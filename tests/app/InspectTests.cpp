#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/model/Inspect.h"
#include "core/Errors.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::AppliedChange Applied(core::ChangeOp op, std::string path)
{
    core::AppliedChange applied;
    applied.change.op = op;
    applied.change.path = std::move(path);
    return applied;
}

} // namespace

TEST_CASE("MarkFor marks changed nodes, what lies inside them, and what contains them", "[inspect]")
{
    const std::vector<core::AppliedChange> applied = {Applied(core::ChangeOp::Set, "player.location"),
                                                      Applied(core::ChangeOp::Add, "player.inventory.lantern"),
                                                      Applied(core::ChangeOp::Create, "npc_1")};
    CHECK(app::MarkFor("player.location", applied) == app::ChangeMark::Changed);
    CHECK(app::MarkFor("player.inventory.lantern", applied) == app::ChangeMark::Changed);
    CHECK(app::MarkFor("player.inventory", applied) == app::ChangeMark::Contains);
    CHECK(app::MarkFor("player", applied) == app::ChangeMark::Contains);
    CHECK(app::MarkFor("npc_1", applied) == app::ChangeMark::Changed);
    CHECK(app::MarkFor("npc_1.disposition", applied) == app::ChangeMark::Changed); // Inside a created entity.
    CHECK(app::MarkFor("player.health", applied) == app::ChangeMark::None);
    CHECK(app::MarkFor("npc_10", applied) == app::ChangeMark::None); // A prefix only on '.' boundaries.
    CHECK(app::MarkFor("player", {}) == app::ChangeMark::None);
}

TEST_CASE("PromptSections titles each message with its layer, and RawPrompt joins them", "[inspect]")
{
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::System, "rules"},
                       {inference::Role::User, "I walk."},
                       {inference::Role::Assistant, "You walk."},
                       {inference::Role::User, "state and input"}};
    prompt.stableCount = 3;

    const std::vector<app::PromptSection> sections = app::PromptSections(prompt);
    REQUIRE(sections.size() == 4);
    CHECK_THAT(sections[0].title, ContainsSubstring("system") && ContainsSubstring("L0"));
    CHECK_THAT(sections[1].title, ContainsSubstring("user") && ContainsSubstring("L3 transcript"));
    CHECK_THAT(sections[2].title, ContainsSubstring("assistant") && ContainsSubstring("L3 transcript"));
    CHECK_THAT(sections[3].title, ContainsSubstring("tail"));
    CHECK(sections[3].text == "state and input");

    CHECK(app::RawPrompt(prompt) == "=== system ===\nrules\n=== user ===\nI walk.\n=== assistant ===\nYou walk.\n"
                                    "=== user ===\nstate and input\n");
}

TEST_CASE("CallStatsOf reads one call's numbers from the telemetry", "[inspect]")
{
    const nlohmann::json timings = {
        {"resolve",
         {{"prompt_tokens", 900}, {"reused", 850}, {"evaluated", 50}, {"generated", 70}, {"prompt_ms", 12.5},
          {"generate_ms", 800.0}, {"stopped_at_limit", false}}},
        {"narrate",
         {{"prompt_tokens", 1000}, {"reused", 990}, {"evaluated", 10}, {"generated", 300}, {"prompt_ms", 3.0},
          {"generate_ms", 2500.0}, {"stopped_at_limit", true}}}};
    const std::optional<app::CallStats> resolve = app::CallStatsOf(timings, "resolve");
    REQUIRE(resolve);
    CHECK(resolve->promptTokens == 900);
    CHECK(resolve->reused == 850);
    CHECK(resolve->evaluated == 50);
    CHECK(resolve->generated == 70);
    CHECK(resolve->promptMs == 12.5);
    CHECK_FALSE(resolve->stoppedAtLimit);
    CHECK(app::CallStatsOf(timings, "narrate")->stoppedAtLimit);
    CHECK_FALSE(app::CallStatsOf(timings, "lookup")); // Engine mode makes no lookup call.
}

TEST_CASE("RetrievalRows shows each hit's text", "[inspect]")
{
    core::TurnRecord first;
    first.number = 1;
    first.input = "I climb the cliff.";
    const std::vector<core::Note> notes = {{1, "The cliff is slick."}, {1, "Gulls nest here."}};
    const nlohmann::json timings = {{"retrieval",
                                     {{{"source", "lore"}, {"ref", "keeper"}, {"score", 0.8}},
                                      {{"source", "history"}, {"ref", "1"}, {"score", 0.6}},
                                      {{"source", "notes"}, {"ref", "1.1"}, {"score", 0.5}}}}};

    const std::vector<app::RetrievalRow> rows = app::RetrievalRows(timings, {first}, notes);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].source == "lore");
    CHECK(rows[0].text == "keeper"); // A lore hit's ref is its subject.
    CHECK(rows[0].score == 0.8);
    CHECK(rows[1].text == "I climb the cliff.");
    CHECK(rows[2].text == "Gulls nest here."); // Turn 1's second note.

    CHECK(app::RetrievalRows(nlohmann::json::object(), {}, {}).empty());
    const nlohmann::json gone = {{"retrieval", {{{"source", "history"}, {"ref", "7"}, {"score", 0.5}}}}};
    CHECK_THROWS_AS(app::RetrievalRows(gone, {first}, notes), core::DataError);
}

TEST_CASE("CheckText writes out the roll", "[inspect]")
{
    core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.result.rolls = {14};
    check.result.statValue = 3;
    check.result.total = 17;
    check.result.target = 16;
    check.result.success = true;
    CHECK(app::CheckText(check) == "agility vs hard: rolled 14 + 3 = 17 against 16, success");

    check.result.rolls = {2, 5};
    check.result.statValue = 0;
    check.result.total = 7;
    check.result.success = false;
    CHECK(app::CheckText(check) == "agility vs hard: rolled 2 + 5 = 7 against 16, failure");
}
