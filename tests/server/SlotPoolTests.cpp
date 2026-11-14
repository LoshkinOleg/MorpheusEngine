#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

#include "core/Errors.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("SlotPool keeps slot 0 for upkeep and gives conversations the others, least recently used first",
          "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.SlotCount() == 3);
    CHECK(pool.Acquire("run/c1") == 1);
    CHECK(pool.Acquire("run/c2") == 2);
    CHECK(pool.Acquire("run/c1") == 1); // Its own slot again; c1 is now the most recently used.
    // A third conversation takes the slot of the one used least recently (c2), which loses it.
    CHECK(pool.Acquire("run/c3") == 2);
    CHECK(pool.HolderOf(2) == "run/c3");
    CHECK(pool.HolderOf(0) == "");
    // c2's next turn takes a slot again: c1's, now the least recently used.
    CHECK(pool.Acquire("run/c2") == 1);
    CHECK(pool.HolderOf(1) == "run/c2");
}

TEST_CASE("SlotPool frees a released conversation's slot for the next one", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.Acquire("a/c1") == 1);
    CHECK(pool.Acquire("a/c2") == 2);
    pool.Release("a/c1");
    CHECK(pool.HolderOf(1) == "");
    CHECK(pool.Acquire("b/c1") == 1);
    pool.Release("never/held"); // Nothing to free: nothing changes.
    CHECK(pool.HolderOf(1) == "b/c1");
    CHECK(pool.HolderOf(2) == "a/c2");
}

TEST_CASE("SlotPool with one slot shares slot 0 between upkeep and every conversation", "[hosted-server][slots]")
{
    server::SlotPool pool(1);
    CHECK(pool.Acquire("a/c1") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.Acquire("a/c2") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.HolderOf(0) == "");
}

TEST_CASE("SlotPool hands out conversation slots safely from several threads", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    std::vector<int> slots(400, -1);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, &slots, t] {
                for (int i = 0; i < 100; ++i) {
                    slots[static_cast<std::size_t>(t * 100 + i)] = pool.Acquire("run/c" + std::to_string(t * 100 + i));
                }
            });
        }
    }
    for (const int slot : slots) {
        CHECK((slot == 1 || slot == 2));
    }
}

TEST_CASE("SlotPool refuses no slot and an empty key", "[hosted-server][slots]")
{
    CHECK_THROWS_AS(server::SlotPool(0), core::ContractError);
    server::SlotPool pool(2);
    CHECK_THROWS_AS(pool.Acquire(""), core::ContractError);
    CHECK_THROWS_AS(pool.HolderOf(2), core::ContractError);
}
