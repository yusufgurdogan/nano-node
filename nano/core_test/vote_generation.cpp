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
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	ASSERT_FALSE (system.nodes[0]->active.generator.add (1, 2));
	ASSERT_TRUE (system.nodes[0]->active.generator.add (1, 2));
	ASSERT_TRUE (system.nodes[0]->active.generator.add (1, 3));
}
