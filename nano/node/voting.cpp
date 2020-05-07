#include <nano/lib/threading.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/udp.hpp>
#include <nano/node/vote_processor.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/blockstore.hpp>

#include <chrono>

nano::vote_reserver::vote_reserver (nano::local_vote_history & history_a) :
history (history_a)
{
}

bool nano::vote_reserver::add (nano::root const & root_a)
{
	clean ();
	auto result = reservations.get<tag_root> ().emplace (root_a).second;
	if (result)
	{
		history.erase (root_a);
	}
	return !result;
}

bool nano::vote_reserver::validate_and_update (std::vector<nano::root> const & roots_a)
{
	clean ();
	auto & reservations_by_root (reservations.get<tag_root> ());
	auto now (std::chrono::steady_clock::now ());
	auto update_time = [&now](auto & reservation_a) { reservation_a.time = now; };
	auto result = std::any_of (roots_a.begin (), roots_a.end (), [&update_time, &reservations_by_root](auto const & root_a) {
		auto existing (reservations_by_root.find (root_a));
		return existing == reservations_by_root.end () || !reservations_by_root.modify (existing, update_time);
	});
	return result;
}

void nano::vote_reserver::clean ()
{
	auto & reservations_by_time (reservations.get<tag_time> ());
	auto cutoff (reservations_by_time.lower_bound (std::chrono::steady_clock::now () - round_time));
	reservations_by_time.erase (reservations_by_time.begin (), cutoff);
}

void nano::local_vote_history::add (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	clean ();
	auto & history_by_root (history.get<tag_root> ());
	// Erase any vote that is not for this hash
	auto range (history_by_root.equal_range (root_a));
	for (auto i (range.first); i != range.second;)
	{
		if (i->hash != hash_a)
		{
			i = history_by_root.erase (i);
		}
		else
		{
			++i;
		}
	}
	auto result (history_by_root.emplace (root_a, hash_a, vote_a));
	debug_assert (result.second);
	debug_assert (std::all_of (history_by_root.equal_range (root_a).first, history_by_root.equal_range (root_a).second, [&hash_a](local_vote const & item_a) -> bool { return item_a.vote != nullptr && item_a.hash == hash_a; }));
}

void nano::local_vote_history::erase (nano::root const & root_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	auto & history_by_root (history.get<tag_root> ());
	auto range (history_by_root.equal_range (root_a));
	history_by_root.erase (range.first, range.second);
}

std::vector<std::shared_ptr<nano::vote>> nano::local_vote_history::votes (nano::root const & root_a) const
{
	nano::lock_guard<std::mutex> guard (mutex);
	std::vector<std::shared_ptr<nano::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	std::transform (range.first, range.second, std::back_inserter (result), [](auto const & entry) { return entry.vote; });
	return result;
}

std::vector<std::shared_ptr<nano::vote>> nano::local_vote_history::votes (nano::root const & root_a, nano::block_hash const & hash_a) const
{
	nano::lock_guard<std::mutex> guard (mutex);
	std::vector<std::shared_ptr<nano::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	// clang-format off
	nano::transform_if (range.first, range.second, std::back_inserter (result),
		[&hash_a](auto const & entry) { return entry.hash == hash_a; },
		[](auto const & entry) { return entry.vote; });
	// clang-format on
	return result;
}

bool nano::local_vote_history::exists (nano::root const & root_a) const
{
	nano::lock_guard<std::mutex> guard (mutex);
	return history.get<tag_root> ().find (root_a) != history.get<tag_root> ().end ();
}

void nano::local_vote_history::clean ()
{
	debug_assert (max_size > 0);
	auto & history_by_sequence (history.get<tag_sequence> ());
	while (history_by_sequence.size () > max_size)
	{
		history_by_sequence.erase (history_by_sequence.begin ());
	}
}

size_t nano::local_vote_history::size () const
{
	nano::lock_guard<std::mutex> guard (mutex);
	return history.size ();
}

nano::vote_generator::vote_generator (nano::node_config & config_a, nano::block_store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a) :
reserver (history_a),
config (config_a),
store (store_a),
wallets (wallets_a),
vote_processor (vote_processor_a),
history (history_a),
network (network_a),
thread ([this]() { run (); })
{
	nano::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

bool nano::vote_generator::add (nano::root const & root_a, nano::block_hash const & hash_a)
{
	auto votes (history.votes (root_a, hash_a));
	auto result = votes.empty ();
	if (!result)
	{
		for (auto const & vote : votes)
		{
			broadcast_action (vote);
		}
	}
	else
	{
		nano::unique_lock<std::mutex> lock (mutex);
		result = reserver.add (root_a);
		if (!result)
		{
			hashes.emplace_back (root_a, hash_a);
			if (hashes.size () >= nano::network::confirm_ack_hashes_max)
			{
				lock.unlock ();
				condition.notify_all ();
			}
		}
	}
	return result;
}

void nano::vote_generator::generate (std::vector<std::pair<nano::block_hash, nano::root>> const & requests_a, std::function<void(std::shared_ptr<nano::vote> const &)> const & action_a)
{
	if (!requests_a.empty ())
	{
		std::vector<nano::block_hash> hashes;
		std::vector<nano::root> roots;
		hashes.reserve (nano::network::confirm_ack_hashes_max);
		roots.reserve (nano::network::confirm_ack_hashes_max);
		nano::unique_lock<std::mutex> lock (mutex);
		for (auto const & request : requests_a)
		{
			auto const & root (request.second);
			if (!reserver.add (root))
			{
				auto const & hash (request.first);
				hashes.push_back (hash);
				roots.push_back (root);
				if (hashes.size () == nano::network::confirm_ack_hashes_max)
				{
					vote (lock, hashes, roots, action_a);
					hashes.clear ();
					roots.clear ();
				}
			}
		}
		if (!hashes.empty ())
		{
			vote (lock, hashes, roots, action_a);
		}
	}
}

void nano::vote_generator::stop ()
{
	nano::unique_lock<std::mutex> lock (mutex);
	stopped = true;

	lock.unlock ();
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_generator::send (nano::unique_lock<std::mutex> & lock_a)
{
	std::vector<nano::block_hash> hashes_l;
	std::vector<nano::root> roots;
	hashes_l.reserve (nano::network::confirm_ack_hashes_max);
	roots.reserve (nano::network::confirm_ack_hashes_max);
	std::unordered_set<std::shared_ptr<nano::vote>> broadcasted_votes;
	while (!hashes.empty () && hashes_l.size () < nano::network::confirm_ack_hashes_max)
	{
		auto front (hashes.front ());
		hashes.pop_front ();
		debug_assert (reserver.add (front.first));
		roots.push_back (front.first);
		hashes_l.push_back (front.second);
	}
	if (!hashes_l.empty ())
	{
		vote (lock_a, hashes_l, roots, [this](auto vote_a) { this->broadcast_action (vote_a); });
	}
}

void nano::vote_generator::vote (nano::unique_lock<std::mutex> & lock_a, std::vector<nano::block_hash> const & hashes_a, std::vector<nano::root> const & roots_a, std::function<void(std::shared_ptr<nano::vote> const &)> const & action_a)
{
	debug_assert (hashes_a.size () == roots_a.size ());
	{
		lock_a.unlock ();
		auto transaction (store.tx_begin_read ());
		std::vector<std::shared_ptr<nano::vote>> votes_l;
		wallets.foreach_representative ([this, &hashes_a, &roots_a, &transaction, &votes_l](nano::public_key const & pub_a, nano::raw_key const & prv_a) {
			votes_l.emplace_back (this->store.vote_generate (transaction, pub_a, prv_a, hashes_a));
		});
		lock_a.lock ();
		// To be strictly correct, validation must go after vote generation. If validation fails, the votes are not used
		if (!reserver.validate_and_update (roots_a))
		{
			lock_a.unlock ();
			for (auto const & vote_l : votes_l)
			{
				for (size_t i (0), n (hashes_a.size ()); i != n; ++i)
				{
					this->history.add (roots_a[i], hashes_a[i], vote_l);
				}
				action_a (vote_l);
			}
			lock_a.lock ();
		}
	}
}

void nano::vote_generator::broadcast_action (std::shared_ptr<nano::vote> const & vote_a) const
{
	network.flood_vote_pr (vote_a);
	network.flood_vote (vote_a, 2.0f);
	vote_processor.vote (vote_a, std::make_shared<nano::transport::channel_udp> (network.udp_channels, network.endpoint (), network_params.protocol.protocol_version));
}

void nano::vote_generator::run ()
{
	nano::thread_role::set (nano::thread_role::name::voting);
	nano::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (hashes.size () >= nano::network::confirm_ack_hashes_max)
		{
			send (lock);
		}
		else
		{
			condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->hashes.size () >= nano::network::confirm_ack_hashes_max; });
			if (hashes.size () >= config.vote_generator_threshold && hashes.size () < nano::network::confirm_ack_hashes_max)
			{
				condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->hashes.size () >= nano::network::confirm_ack_hashes_max; });
			}
			if (!hashes.empty ())
			{
				send (lock);
			}
		}
	}
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::local_vote_history & history, const std::string & name)
{
	size_t history_count = history.size ();
	auto sizeof_element = sizeof (decltype (history.history)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	/* This does not currently loop over each element inside the cache to get the sizes of the votes inside cached_votes */
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "history", history_count, sizeof_element }));
	return composite;
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::vote_generator & vote_generator, const std::string & name)
{
	size_t hashes_count = 0;
	size_t reservation_count = 0;

	{
		nano::lock_guard<std::mutex> guard (vote_generator.mutex);
		hashes_count = vote_generator.hashes.size ();
		reservation_count = vote_generator.reserver.reservations.size ();
	}
	auto sizeof_hashes_element = sizeof (decltype (vote_generator.hashes)::value_type);
	auto sizeof_reservation_element = sizeof (decltype (vote_generator.reserver.reservations)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "hashes", hashes_count, sizeof_hashes_element }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "reservation_count", hashes_count, sizeof_reservation_element }));
	return composite;
}
