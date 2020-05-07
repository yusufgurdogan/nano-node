#include <nano/core_test/testutil.hpp>
#include <nano/node/common.hpp>
#include <nano/node/testing.hpp>
#include <nano/node/voting.hpp>

using namespace std::chrono_literals;

namespace nano
{
TEST (local_vote_history, basic)
{
	nano::local_vote_history history;
	ASSERT_FALSE (history.exists (1));
	ASSERT_FALSE (history.exists (2));
	ASSERT_TRUE (history.votes (1).empty ());
	ASSERT_TRUE (history.votes (2).empty ());
	auto vote1 (std::make_shared<nano::vote> ());
	ASSERT_EQ (0, history.size ());
	history.add (1, 2, vote1);
	ASSERT_EQ (1, history.size ());
	ASSERT_TRUE (history.exists (1));
	ASSERT_FALSE (history.exists (2));
	auto votes1 (history.votes (1));
	ASSERT_FALSE (votes1.empty ());
	ASSERT_EQ (1, history.votes (1, 2).size ());
	ASSERT_TRUE (history.votes (1, 1).empty ());
	ASSERT_TRUE (history.votes (1, 3).empty ());
	ASSERT_TRUE (history.votes (2).empty ());
	ASSERT_EQ (1, votes1.size ());
	ASSERT_EQ (vote1, votes1[0]);
	auto vote2 (std::make_shared<nano::vote> ());
	ASSERT_EQ (1, history.size ());
	history.add (1, 2, vote2);
	ASSERT_EQ (2, history.size ());
	auto votes2 (history.votes (1));
	ASSERT_EQ (2, votes2.size ());
	ASSERT_TRUE (vote1 == votes2[0] || vote1 == votes2[1]);
	ASSERT_TRUE (vote2 == votes2[0] || vote2 == votes2[1]);
	auto vote3 (std::make_shared<nano::vote> ());
	history.add (1, 3, vote3);
	ASSERT_EQ (1, history.size ());
	auto votes3 (history.votes (1));
	ASSERT_EQ (1, votes3.size ());
	ASSERT_TRUE (vote3 == votes3[0]);
}
}

TEST (vote_reserver, basic)
{
	nano::local_vote_history history;
	nano::vote_reserver reserver (history);
	ASSERT_FALSE (history.exists (1));
	history.add (1, 2, std::make_shared<nano::vote> ());
	ASSERT_TRUE (history.exists (1));
	ASSERT_FALSE (reserver.add (1));
	ASSERT_FALSE (history.exists (1));
	history.add (1, 2, std::make_shared<nano::vote> ());
	ASSERT_TRUE (history.exists (1));
	ASSERT_TRUE (reserver.add (1));
	ASSERT_TRUE (history.exists (1));
	ASSERT_FALSE (reserver.add (2));
	ASSERT_TRUE (reserver.add (1));
	auto iterations (0);
	while (reserver.add (1))
	{
		ASSERT_TRUE (history.exists (1));
		std::this_thread::sleep_for (100ms);
		ASSERT_LT (++iterations, 20);
	}
	ASSERT_FALSE (history.exists (1));
	ASSERT_GT (iterations, 0);
	ASSERT_TRUE (reserver.add (1));
	ASSERT_FALSE (reserver.add (2));
	ASSERT_TRUE (reserver.add (1));
}

TEST (vote_generator, cache)
{
	nano::system system (1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	auto & node (*system.nodes[0]);
	ASSERT_FALSE (node.active.generator.add (1, 2));
	ASSERT_TIMELY (1s, !node.history.votes (1, 2).empty ());
	auto votes (node.history.votes (1, 2));
	ASSERT_FALSE (votes.empty ());
	ASSERT_TRUE (std::any_of (votes[0]->begin (), votes[0]->end (), [](nano::block_hash const & hash_a) { return hash_a == 2; }));
}

TEST (vote_generator, duplicate)
{
	nano::system system (1);
	auto & node (*system.nodes[0]);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	ASSERT_FALSE (node.active.generator.add (1, 2));
	ASSERT_TRUE (node.active.generator.add (1, 2));
	ASSERT_TRUE (node.active.generator.add (1, 3));
}

namespace nano
{
TEST (vote_generator, spacing)
{
	auto const N = nano::Gxrb_ratio;
	nano::system system;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.online_weight_minimum = 2 * N;
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node (*system.add_node (config));

	ASSERT_GE (N, node.config.vote_minimum.number ());

	nano::keypair key1; // rep with weight N
	nano::keypair key2; // rep with weight 2N

	nano::block_builder builder;
	std::error_code ec;

	auto send1 = builder.state ()
	             .account (nano::test_genesis_key.pub)
	             .previous (nano::genesis_hash)
	             .representative (nano::test_genesis_key.pub)
	             .balance (node.balance (nano::test_genesis_key.pub) - N)
	             .link (key1.pub)
	             .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	             .work (*system.work.generate (nano::genesis_hash))
	             .build (ec);
	ASSERT_FALSE (ec);

	auto open1 = builder.state ()
	             .account (key1.pub)
	             .previous (0)
	             .representative (key1.pub)
	             .balance (N)
	             .link (send1->hash ())
	             .sign (key1.prv, key1.pub)
	             .work (*system.work.generate (key1.pub))
	             .build (ec);
	ASSERT_FALSE (ec);

	ASSERT_EQ (nano::process_result::progress, node.process (*send1).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open1).code);
	ASSERT_EQ (N, node.weight (key1.pub));

	auto send2 = builder.state ()
	             .account (nano::test_genesis_key.pub)
	             .previous (send1->hash ())
	             .representative (nano::test_genesis_key.pub)
	             .balance (node.balance (nano::test_genesis_key.pub) - 2 * N)
	             .link (key2.pub)
	             .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	             .work (*system.work.generate (send1->hash ()))
	             .build (ec);
	ASSERT_FALSE (ec);

	auto open2 = builder.state ()
	             .account (key2.pub)
	             .previous (0)
	             .representative (key2.pub)
	             .balance (2 * N)
	             .link (send2->hash ())
	             .sign (key2.prv, key2.pub)
	             .work (*system.work.generate (key2.pub))
	             .build (ec);
	ASSERT_FALSE (ec);

	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open2).code);
	ASSERT_EQ (2 * N, node.weight (key2.pub));

	std::shared_ptr<nano::state_block> send3 = builder.state ()
	                                           .account (nano::test_genesis_key.pub)
	                                           .previous (send2->hash ())
	                                           .representative (nano::test_genesis_key.pub)
	                                           .balance (node.balance (nano::test_genesis_key.pub) - 10)
	                                           .link (nano::test_genesis_key.pub)
	                                           .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                           .work (*system.work.generate (send2->hash ()))
	                                           .build (ec);
	ASSERT_FALSE (ec);

	std::shared_ptr<nano::state_block> send3_fork = builder.state ()
	                                                .account (nano::test_genesis_key.pub)
	                                                .previous (send2->hash ())
	                                                .representative (nano::test_genesis_key.pub)
	                                                .balance (node.balance (nano::test_genesis_key.pub) - 20)
	                                                .link (nano::test_genesis_key.pub)
	                                                .sign (nano::test_genesis_key.prv, nano::test_genesis_key.pub)
	                                                .work (*system.work.generate (send2->hash ()))
	                                                .build (ec);
	ASSERT_FALSE (ec);

	ASSERT_NE (send3->hash (), send3_fork->hash ());

	// Start an election for send3, then publish send3_fork as welll
	ASSERT_EQ (nano::process_result::progress, node.process_local (send3).code);
	ASSERT_EQ (nano::process_result::fork, node.process_local (send3_fork).code);
	auto election = node.active.election (send3->qualified_root ());
	ASSERT_NE (nullptr, election);
	ASSERT_EQ (2, election->blocks.size ());

	// Insert key1 into wallet
	system.wallet (0)->insert_adhoc (key1.prv);
	node.wallets.compute_reps ();
	ASSERT_EQ (1, node.wallets.rep_counts ().voting);

	// Generate a vote for send3
	ASSERT_FALSE (election->need_vote);
	ASSERT_FALSE (node.active.generator.add (send3->root (), send3->hash ()));

	// Wait for the vote
	ASSERT_NO_ERROR (system.poll_until_true (3s, [&election, &node, &key1, hash = send3->hash ()]() {
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		auto existing (election->last_votes.find (key1.pub));
		return existing != election->last_votes.end () && existing->second.hash == hash;
	}));
	auto vote1 = election->last_votes.at (key1.pub);

	// Vote with key2 which is enough to switch the vote of key1
	ASSERT_FALSE (election->need_vote);
	{
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		election->vote (key2.pub, vote1.time.time_since_epoch ().count () + 1, send3_fork->hash ());
	}
	ASSERT_TRUE (election->need_vote);

	// Wait for the vote
	ASSERT_NO_ERROR (system.poll_until_true (3s, [&election, &node, &key1, hash_fork = send3_fork->hash ()]() {
		nano::lock_guard<std::mutex> guard (node.active.mutex);
		auto existing (election->last_votes.find (key1.pub));
		return existing != election->last_votes.end () && existing->second.hash == hash_fork;
	}));
	auto vote1_fork = election->last_votes.at (key1.pub);

	// Ensure enough time has passed between the two votes
	ASSERT_GT (vote1_fork.time - vote1.time, node.active.generator.reserver.round_time);
	ASSERT_FALSE (election->need_vote);
}
}
