#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>

#include "core/Errors.h"
#include "server/TurnStream.h"

using namespace morpheus;
using namespace std::chrono_literals;

TEST_CASE("TurnStream hands events over in order, then says it ended", "[hosted-server][stream]")
{
    server::TurnStream stream;
    CHECK_FALSE(stream.WaitStarted(10ms));
    CHECK_FALSE(stream.Front().has_value());
    std::jthread worker([&stream] {
        stream.Push({"action", {{"speaker", "a"}}});
        stream.Push({"token", {{"text", "Hi"}}});
        stream.End();
    });
    CHECK(stream.WaitStarted(5000ms));
    REQUIRE(stream.Front().has_value());
    CHECK(stream.Front()->name == "action"); // Front does not take it.
    std::optional<server::StreamEvent> first = stream.Next(5000ms);
    REQUIRE(first);
    CHECK(first->name == "action");
    worker.join();
    std::optional<server::StreamEvent> second = stream.Next(5000ms);
    REQUIRE(second);
    CHECK(second->data.at("text") == "Hi");
    CHECK(stream.Ended());
    CHECK_FALSE(stream.Next(10ms).has_value());
    CHECK_THROWS_AS(stream.Push({"token", {}}), core::ContractError);
}

TEST_CASE("TurnStream::Cancel fires its stop token", "[hosted-server][stream]")
{
    server::TurnStream stream;
    const std::stop_token token = stream.StopToken();
    CHECK_FALSE(token.stop_requested());
    stream.Cancel();
    CHECK(token.stop_requested());
}
