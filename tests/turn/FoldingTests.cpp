#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "context/Transcript.h"
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

// A turn with one check and one applied change, so its mechanics line is not empty.
core::TurnRecord MechanicalTurn(int number, const std::string& input)
{
    core::TurnRecord record = Turn(number, input, "Narration.");
    core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks.push_back(check);
    core::AppliedChange applied;
    applied.change.op = core::ChangeOp::Add;
    applied.change.path = "player.health";
    applied.change.value = -3;
    record.applied.push_back(applied);
    return record;
}

// A backend whose model writes only whitespace.
test::ScriptedBackend BlankBackend()
{
    return test::ScriptedBackend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("  \n\t ");
    });
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

    // The two cancelled calls reached the backend; the rejected calls must not add any.
    const std::size_t callsBefore = backend.Calls().size();
    CHECK_THROWS_AS(turn::SummarizeTurns({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
    CHECK_THROWS_AS(turn::CondenseSummaries({inference::Role::System, "S"}, {}, {}, backend, {}), core::ContractError);
    CHECK(backend.Calls().size() == callsBefore);
}

TEST_CASE("Folding calls reject a message that is not a system message", "[folding]")
{
    test::ScriptedBackend backend(16384, [](const inference::Prompt&, const inference::GenerationSpec&) {
        return std::string("Summary.");
    });
    const inference::Message user = {inference::Role::User, "not a system message"};
    CHECK_THROWS_AS(turn::SummarizeTurns(user, {Turn(1, "a", "b")}, {}, backend, {}), core::ContractError);
    CHECK_THROWS_AS(turn::CondenseSummaries(user, {{1, 1, "a"}}, {}, backend, {}), core::ContractError);
    CHECK(backend.Calls().empty());
}

TEST_CASE("SummarizeTurns writes a factual fallback when the model writes nothing", "[folding]")
{
    test::ScriptedBackend backend = BlankBackend();
    const std::vector<core::TurnRecord> turns = {Turn(3, "I knock.", "The door creaks."),
                                                 MechanicalTurn(4, "I jump the gap.")};
    const std::string mechanics = context::RenderMechanicsLine(turns[1]);
    REQUIRE_FALSE(mechanics.empty());

    const std::optional<core::Summary> summary =
        turn::SummarizeTurns({inference::Role::System, "S"}, turns, inference::GenerationSpec{}, backend, {});
    REQUIRE(summary);
    CHECK(summary->fromTurn == 3);
    CHECK(summary->toTurn == 4);
    CHECK(summary->text == "Turn 3: I knock.\nTurn 4: I jump the gap. " + mechanics);
}

TEST_CASE("CondenseSummaries writes a factual fallback when the model writes nothing", "[folding]")
{
    test::ScriptedBackend backend = BlankBackend();
    const std::optional<core::Summary> merged = turn::CondenseSummaries(
        {inference::Role::System, "S"}, {{1, 3, "The keeper lit the lamp."}, {4, 9, "The ship sank."}},
        inference::GenerationSpec{}, backend, {});
    REQUIRE(merged);
    CHECK(merged->fromTurn == 1);
    CHECK(merged->toTurn == 9);
    CHECK(merged->text == "Turns 1-3: The keeper lit the lamp.\nTurns 4-9: The ship sank.");
}

TEST_CASE("A fallback summary is cut to about four bytes per token of the cap", "[folding]")
{
    test::ScriptedBackend backend = BlankBackend();
    inference::GenerationSpec spec;
    spec.maxTokens = 10;

    const std::optional<core::Summary> summary =
        turn::SummarizeTurns({inference::Role::System, "S"}, {Turn(1, std::string(200, 'x'), "b")}, spec, backend, {});
    REQUIRE(summary);
    CHECK(summary->text == "Turn 1: " + std::string(32, 'x')); // 40 bytes in all

    const std::optional<core::Summary> merged =
        turn::CondenseSummaries({inference::Role::System, "S"}, {{1, 2, std::string(200, 'y')}}, spec, backend, {});
    REQUIRE(merged);
    CHECK(merged->text == "Turns 1-2: " + std::string(29, 'y')); // 40 bytes in all
}

TEST_CASE("The fold and condense prompts and fallbacks are the ones the folding calls use", "[folding]")
{
    const inference::Message system = {inference::Role::System, "S"};
    const std::vector<core::TurnRecord> turns = {Turn(3, "I knock.", "The door creaks."),
                                                 MechanicalTurn(4, "I jump the gap.")};
    const std::vector<core::Summary> summaries = {{1, 3, "The keeper lit the lamp."}, {4, 9, "The ship sank."}};
    inference::GenerationSpec spec;
    spec.maxTokens = 10;

    // The prompts the session measures before a call are the prompts the calls send.
    test::ScriptedBackend backend = BlankBackend();
    const std::optional<core::Summary> summary = turn::SummarizeTurns(system, turns, spec, backend, {});
    const std::optional<core::Summary> merged = turn::CondenseSummaries(system, summaries, spec, backend, {});
    REQUIRE(backend.Calls().size() == 2);
    const inference::Prompt summarizePrompt = turn::SummarizePrompt(system, turns);
    const inference::Prompt condensePrompt = turn::CondensePrompt(system);
    REQUIRE(backend.Calls()[0].prompt.messages.size() == summarizePrompt.messages.size());
    for (std::size_t i = 0; i < summarizePrompt.messages.size(); ++i) {
        CHECK(backend.Calls()[0].prompt.messages[i].content == summarizePrompt.messages[i].content);
    }
    CHECK(backend.Calls()[0].prompt.stableCount == summarizePrompt.stableCount);
    REQUIRE(backend.Calls()[1].prompt.messages.size() == condensePrompt.messages.size());
    CHECK(backend.Calls()[1].prompt.messages.back().content == condensePrompt.messages.back().content);
    CHECK(backend.Calls()[1].prompt.stableCount == condensePrompt.stableCount);

    // The fallbacks, written without a model call, are what the calls write when the model writes nothing.
    REQUIRE(summary);
    REQUIRE(merged);
    const core::Summary summaryFallback = turn::SummarizeFallback(turns, spec.maxTokens);
    const core::Summary condenseFallback = turn::CondenseFallback(summaries, spec.maxTokens);
    CHECK(summaryFallback.fromTurn == 3);
    CHECK(summaryFallback.toTurn == 4);
    CHECK(summaryFallback.text == summary->text);
    CHECK(condenseFallback.fromTurn == 1);
    CHECK(condenseFallback.toTurn == 9);
    CHECK(condenseFallback.text == merged->text);
    CHECK_THROWS_AS(turn::SummarizeFallback({}, 10), core::ContractError);
    CHECK_THROWS_AS(turn::CondenseFallback({}, 10), core::ContractError);
}
