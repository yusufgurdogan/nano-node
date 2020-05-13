#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/request_aggregator.hpp>
#include <nano/node/transport/udp.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/blockstore.hpp>

nano::request_aggregator::request_aggregator (nano::network_constants const & network_constants_a, nano::node_config const & config_a, nano::stat & stats_a, nano::vote_generator & generator_a, nano::block_store & store_a, nano::wallets & wallets_a, nano::active_transactions & active_a, nano::local_vote_history & local_votes_a) :
max_delay (network_constants_a.is_test_network () ? 50 : 300),
small_delay (network_constants_a.is_test_network () ? 10 : 50),
max_channel_requests (config_a.max_queued_requests),
stats (stats_a),
generator (generator_a),
store (store_a),
wallets (wallets_a),
active (active_a),
local_votes (local_votes_a),
thread ([this]() { run (); })
{
	nano::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void nano::request_aggregator::add (std::shared_ptr<nano::transport::channel> & channel_a, std::vector<std::pair<nano::block_hash, nano::root>> const & hashes_roots_a)
{
	debug_assert (wallets.reps ().voting > 0);
	bool error = true;
	auto const endpoint (nano::transport::map_endpoint_to_v6 (channel_a->get_endpoint ()));
	nano::unique_lock<std::mutex> lock (mutex);
	// Protecting from ever-increasing memory usage when request are consumed slower than generated
	// Reject request if the oldest request has not yet been processed after its deadline + a modest margin
	if (requests.empty () || (requests.get<tag_deadline> ().begin ()->deadline + 2 * this->max_delay > std::chrono::steady_clock::now ()))
	{
		auto & requests_by_endpoint (requests.get<tag_endpoint> ());
		auto existing (requests_by_endpoint.find (endpoint));
		if (existing == requests_by_endpoint.end ())
		{
			existing = requests_by_endpoint.emplace (channel_a).first;
		}
		requests_by_endpoint.modify (existing, [&hashes_roots_a, &channel_a, &error, this](channel_pool & pool_a) {
			// This extends the lifetime of the channel, which is acceptable up to max_delay
			pool_a.channel = channel_a;
			if (pool_a.hashes_roots.size () + hashes_roots_a.size () <= this->max_channel_requests)
			{
				error = false;
				auto new_deadline (std::min (pool_a.start + this->max_delay, std::chrono::steady_clock::now () + this->small_delay));
				pool_a.deadline = new_deadline;
				pool_a.hashes_roots.insert (pool_a.hashes_roots.begin (), hashes_roots_a.begin (), hashes_roots_a.end ());
			}
		});
		if (requests.size () == 1)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
	stats.inc (nano::stat::type::aggregator, !error ? nano::stat::detail::aggregator_accepted : nano::stat::detail::aggregator_dropped);
}

void nano::request_aggregator::run ()
{
	nano::thread_role::set (nano::thread_role::name::request_aggregator);
	nano::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (!requests.empty ())
		{
			auto & requests_by_deadline (requests.get<tag_deadline> ());
			auto front (requests_by_deadline.begin ());
			if (front->deadline < std::chrono::steady_clock::now ())
			{
				// Store the channel and requests for processing after erasing this pool
				decltype (front->channel) channel{};
				decltype (front->hashes_roots) hashes_roots{};
				requests_by_deadline.modify (front, [&channel, &hashes_roots](channel_pool & pool) {
					channel.swap (pool.channel);
					hashes_roots.swap (pool.hashes_roots);
				});
				requests_by_deadline.erase (front);
				lock.unlock ();
				normalize_requests (hashes_roots, channel);
				send_cached (hashes_roots, channel);
				generator.generate (hashes_roots, [this, &channel](auto vote_a) {
					this->reply_action (vote_a, channel);
					this->stats.inc (nano::stat::type::requests, nano::stat::detail::requests_generated_votes, stat::dir::in);
				});
				stats.add (nano::stat::type::requests, nano::stat::detail::requests_generated_hashes, stat::dir::in, hashes_roots.size ());
				lock.lock ();
			}
			else
			{
				auto deadline = front->deadline;
				condition.wait_until (lock, deadline, [this, &deadline]() { return this->stopped || deadline < std::chrono::steady_clock::now (); });
			}
		}
		else
		{
			condition.wait_for (lock, small_delay, [this]() { return this->stopped || !this->requests.empty (); });
		}
	}
}

void nano::request_aggregator::stop ()
{
	{
		nano::lock_guard<std::mutex> guard (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::size_t nano::request_aggregator::size ()
{
	nano::unique_lock<std::mutex> lock (mutex);
	return requests.size ();
}

bool nano::request_aggregator::empty ()
{
	return size () == 0;
}

void nano::request_aggregator::reply_action (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	nano::confirm_ack confirm (vote_a);
	channel_a->send (confirm);
}

void nano::request_aggregator::normalize_requests (std::vector<std::pair<nano::block_hash, nano::root>> & requests_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	auto transaction (store.tx_begin_read ());
	for (auto i (requests_a.begin ()); i != requests_a.end ();)
	{
		auto & hash (i->first);
		auto & root (i->second);
		auto winner (active.winner (hash));
		if (winner.is_initialized ())
		{
			hash = *winner;
			++i;
		}
		else if (store.block_exists (transaction, hash))
		{
			++i;
		}
		else
		{
			auto successor (store.block_successor (transaction, root));
			if (successor.is_zero ())
			{
				nano::account_info info;
				auto error (store.account_get (transaction, root, info));
				if (!error)
				{
					successor = info.open_block;
				}
			}
			if (!successor.is_zero ())
			{
				++i;
				if (hash != successor)
				{
					auto successor_block (store.block_get (transaction, successor));
					debug_assert (successor_block != nullptr);
					nano::publish publish (successor_block);
					channel_a->send (publish);
					hash = successor;
				}
			}
			else
			{
				i = requests_a.erase (i);
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_unknown, stat::dir::in);
			}
		}
	}
}

void nano::request_aggregator::send_cached (std::vector<std::pair<nano::block_hash, nano::root>> & requests_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	std::unordered_set<std::shared_ptr<nano::vote>> sent_votes;
	requests_a.erase (std::remove_if (requests_a.begin (), requests_a.end (), [this, &channel_a, &sent_votes](std::pair<nano::block_hash, nano::root> const & request) {
		auto votes (this->local_votes.votes (request.second, request.first));
		bool result = !votes.empty ();
		if (result)
		{
			for (auto const & vote : votes)
			{
				if (sent_votes.insert (vote).second)
				{
					reply_action (vote, channel_a);
					this->stats.inc (nano::stat::type::requests, nano::stat::detail::requests_cached_votes, stat::dir::in);
				}
			}
			this->stats.inc (nano::stat::type::requests, nano::stat::detail::requests_cached_hashes, stat::dir::in);
		}
		return result;
	}),
	requests_a.end ());
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::request_aggregator & aggregator, const std::string & name)
{
	auto pools_count = aggregator.size ();
	auto sizeof_element = sizeof (decltype (aggregator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pools", pools_count, sizeof_element }));
	return composite;
}
