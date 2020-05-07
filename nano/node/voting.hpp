#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class block_store;
class network;
class node_config;
class vote_processor;
class wallets;

class local_vote_history final
{
	class local_vote final
	{
	public:
		local_vote (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a) :
		root (root_a),
		hash (hash_a),
		vote (vote_a)
		{
		}
		nano::root root;
		nano::block_hash hash;
		std::shared_ptr<nano::vote> vote;
	};

public:
	void add (nano::root const & root_a, nano::block_hash const & hash_a, std::shared_ptr<nano::vote> const & vote_a);
	void erase (nano::root const & root_a);

	std::vector<std::shared_ptr<nano::vote>> votes (nano::root const & root_a, nano::block_hash const & hash_a) const;
	bool exists (nano::root const &) const;
	size_t size () const;

	constexpr static size_t max_size{ 100'000 };

private:
	// clang-format off
	boost::multi_index_container<local_vote,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<class tag_root>,
			mi::member<local_vote, nano::root, &local_vote::root>>,
		mi::sequenced<mi::tag<class tag_sequence>>>>
	history;
	// clang-format on

	void clean ();
	std::vector<std::shared_ptr<nano::vote>> votes (nano::root const & root_a) const;
	mutable std::mutex mutex;

	friend std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, const std::string & name);

	friend class local_vote_history_basic_Test;
};

std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, const std::string & name);

class vote_generator;
class vote_reserver final
{
	class vote_reservation final
	{
	public:
		vote_reservation (nano::root const & root_a) :
		root (root_a),
		time (std::chrono::steady_clock::now ())
		{
		}
		nano::root root;
		std::chrono::steady_clock::time_point const time;
	};

public:
	vote_reserver (nano::local_vote_history &);
	bool add (nano::root const &);
	void clean ();

	std::chrono::seconds const round_time{ nano::network_params ().network.is_test_network () ? 1 : 45 };

private:
	// clang-format off
	boost::multi_index_container<vote_reservation,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<class tag_root>,
			mi::member<vote_reservation, nano::root, &vote_reservation::root>>,
		mi::ordered_non_unique<mi::tag<class tag_time>,
			mi::member<vote_reservation, std::chrono::steady_clock::time_point const, &vote_reservation::time>>>>
	reservations;
	// clang-format on

	nano::local_vote_history & history;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_generator & generator, const std::string & name);
};

class vote_generator final
{
public:
	vote_generator (nano::node_config & config_a, nano::block_store & store_a, nano::wallets & wallets_a, nano::vote_processor & vote_processor_a, nano::local_vote_history & history_a, nano::network & network_a);
	bool add (nano::root const &, nano::block_hash const &);
	/** Generate votes from \p requests_a and apply \p action_a to each */
	void generate (std::vector<std::pair<nano::block_hash, nano::root>> const & requests_a, std::function<void(std::shared_ptr<nano::vote> const &)> const & action_a);
	void stop ();

private:
	std::deque<std::pair<nano::root, nano::block_hash>> hashes;
	nano::vote_reserver reserver;

private:
	void run ();
	void send (nano::unique_lock<std::mutex> &);
	void vote (std::vector<nano::block_hash> const &, std::vector<nano::root> const &, std::function<void(std::shared_ptr<nano::vote> const &)> const &) const;
	void broadcast_action (std::shared_ptr<nano::vote> const &) const;
	mutable std::mutex mutex;
	nano::condition_variable condition;
	nano::node_config & config;
	nano::block_store & store;
	nano::wallets & wallets;
	nano::vote_processor & vote_processor;
	nano::local_vote_history & history;
	nano::network & network;
	nano::network_params network_params;
	bool stopped{ false };
	bool started{ false };
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_generator & generator, const std::string & name);

	friend class vote_generator_spacing_Test;
};

std::unique_ptr<container_info_component> collect_container_info (vote_generator & generator, const std::string & name);
class cached_votes final
{
public:
	nano::block_hash hash;
	std::vector<std::shared_ptr<nano::vote>> votes;
};
}
