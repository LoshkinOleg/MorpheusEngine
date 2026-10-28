#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/Folding.h"

using namespace morpheus;

namespace {

core::TurnRecord Turn(int number, const std::string& input, const std::string& narration)
{
    core::TurnRecord record;
    record.number = number;
    record.input = input;
    record.narration = narration;
    return record;
}

} // namespace

TEST_CASE("SummarizeTurns folds turns after the system message and trims the summary", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("  The keeper lit the lamp.\n");
    });
    const inference::Message system = {inference::Role::System, "SYSTEM"};
    inference::GenerationSpec spec;
    spec.maxTokens = 64;

    const std::optional<core::Summary> summary = turn::SummarizeTurns(
        system, {Turn(3, "I knock.", "The door creaks."), Turn(4, "I enter.", "It is dark.")}, spec, backend, {});
    REQUIRE(summary);
    CHECK(summary->fromTurn == 3);
    CHECK(summary->toTurn == 4);
    CHECK(summary->text == "The keeper lit the lamp.");

    const inference::Prompt& prompt = backend.Calls().at(0).prompt;
    REQUIRE(prompt.messages.size() == 6); // system, 2 x (user, assistant), instruction
    CHECK(prompt.messages[0].content == "SYSTEM");
    CHECK(prompt.messages[1].content == "I knock.");
    CHECK(prompt.messages[4].content == "It is dark.");
    CHECK(prompt.messages[5].content == std::string(turn::SUMMARIZE_INSTRUCTION));
    CHECK(prompt.stableCount == 5);
    CHECK(backend.Calls().at(0).spec.grammar.empty());
}

TEST_CASE("CondenseSummaries covers every summary's range", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("All of it.");
    });
    const std::optional<core::Summary> merged = turn::CondenseSummaries(
        {inference::Role::System, "SYSTEM"}, {{1, 3, "a"}, {4, 9, "b"}}, inference::GenerationSpec{}, backend, {});
    REQUIRE(merged);
    CHECK(merged->fromTurn == 1);
    CHECK(merged->toTurn == 9);
    CHECK(merged->text == "All of it.");
    const inference::Prompt& prompt = backend.Calls().at(0).prompt;
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.messages[1].content == std::string(turn::CONDENSE_INSTRUCTION));
}

TEST_CASE("Folding calls return nothing when cancelled and reject empty input", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("Summary.");
    });
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, "a", "b")}, {}, backend, stop.get_token()));
    CHECK_FALSE(turn::CondenseSummaries({inference::Role::System, "S"}, {{1, 1, "a"}}, {}, backend, stop.get_token()));
    CHECK_THROWS_AS(turn::SummarizeTurns({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
    CHECK_THROWS_AS(turn::CondenseSummaries({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
}
