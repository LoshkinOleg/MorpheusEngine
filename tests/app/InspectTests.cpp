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
          {"generate_ms", 2500.0}, {"stopped_at_limit", true},
          {"thinking", {{"tokens", 640}, {"ms", 90000.0}, {"stopped_at_limit", true}, {"text", "Slow the scene."}}}}}};
    const std::optional<app::CallStats> resolve = app::CallStatsOf(timings, "resolve");
    REQUIRE(resolve);
    CHECK(resolve->promptTokens == 900);
    CHECK(resolve->reused == 850);
    CHECK(resolve->evaluated == 50);
    CHECK(resolve->generated == 70);
    CHECK(resolve->promptMs == 12.5);
    CHECK_FALSE(resolve->stoppedAtLimit);
    CHECK_FALSE(resolve->thinking); // The resolve call did not think.
    const std::optional<app::CallStats> narrate = app::CallStatsOf(timings, "narrate");
    REQUIRE(narrate);
    CHECK(narrate->stoppedAtLimit);
    REQUIRE(narrate->thinking);
    CHECK(narrate->thinking->tokens == 640);
    CHECK(narrate->thinking->ms == 90000.0);
    CHECK(narrate->thinking->stoppedAtLimit);
    CHECK(narrate->thinking->text == "Slow the scene.");
    CHECK_FALSE(app::CallStatsOf(timings, "lookup")); // Engine mode makes no lookup call.
}

TEST_CASE("RetrievalRows shows each hit's text", "[inspect]")
{
    core::TurnRecord first;
    first.number = 1;
    first.input = "I climb the cliff.";
    const std::vector<core::Note> notes = {{1, "The cliff is slick."}, {1, "Gulls nest here."}};
    const nlohmann::json timings = nlohmann::json::parse(R"({"retrieval": [
        {"source": "lore", "ref": "keeper", "score": 0.8, "reasons": ["name", "scene"]},
        {"source": "history", "ref": "1", "score": 0.6, "reasons": []},
        {"source": "notes", "ref": "1.1", "score": 0.5, "reasons": []}]})");

    const std::vector<app::RetrievalRow> rows = app::RetrievalRows(timings, {first}, notes);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].source == "lore");
    CHECK(rows[0].text == "keeper"); // A lore hit's ref is its subject.
    CHECK(rows[0].score == 0.8);
    CHECK(rows[0].reasons == std::vector<std::string>{"name", "scene"});
    CHECK(rows[1].reasons.empty());
    CHECK(rows[1].text == "I climb the cliff.");
    CHECK(rows[2].text == "Gulls nest here."); // Turn 1's second note.

    CHECK(app::RetrievalRows(nlohmann::json::object(), {}, {}).empty());
    const nlohmann::json gone =
        nlohmann::json::parse(R"({"retrieval": [{"source": "history", "ref": "7", "score": 0.5, "reasons": []}]})");
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

TEST_CASE("ReviewRows and ReplacedNarration read a renarrated turn's reviews", "[inspect]")
{
    const nlohmann::json timings = nlohmann::json::parse(R"({
        "review": [
            {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 40, "prompt_ms": 5.0,
             "generate_ms": 300.0, "stopped_at_limit": false,
             "consistent": false, "problems": ["The climb failed."], "digest": "The player reached the top."},
            {"prompt_tokens": 410, "reused": 0, "evaluated": 410, "generated": 30, "prompt_ms": 5.0,
             "generate_ms": 250.0, "stopped_at_limit": false, "consistent": true, "digest": "The player slipped.",
             "thinking": {"tokens": 70, "ms": 900.0, "stopped_at_limit": false, "text": "Check the roll."}}],
        "renarrate": {"prompt_tokens": 1200, "reused": 1100, "evaluated": 100, "generated": 90, "prompt_ms": 20.0,
                      "generate_ms": 1500.0, "stopped_at_limit": false, "replaced": "You reach the top."}})");

    const std::vector<app::ReviewRow> rows = app::ReviewRows(timings);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].call.promptTokens == 400);
    CHECK(rows[0].consistent == false);
    CHECK(rows[0].problems == std::vector<std::string>{"The climb failed."});
    CHECK(rows[0].digest == "The player reached the top.");
    CHECK_FALSE(rows[0].call.thinking);
    CHECK(rows[1].consistent == true);
    CHECK(rows[1].problems.empty());
    REQUIRE(rows[1].call.thinking);
    CHECK(rows[1].call.thinking->text == "Check the roll.");
    CHECK(app::ReplacedNarration(timings) == "You reach the top.");
    const std::optional<app::CallStats> renarrate = app::CallStatsOf(timings, "renarrate");
    REQUIRE(renarrate);
    CHECK(renarrate->generated == 90);

    // An unreadable review keeps only its reason; a turn without reviews has none.
    const std::vector<app::ReviewRow> failed = app::ReviewRows(nlohmann::json::parse(R"({"review": [
        {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 384, "prompt_ms": 5.0,
         "generate_ms": 3000.0, "stopped_at_limit": true, "error": "the review output is not complete JSON"}]})"));
    REQUIRE(failed.size() == 1);
    CHECK_FALSE(failed[0].consistent);
    CHECK(failed[0].call.stoppedAtLimit);
    CHECK(failed[0].error == "the review output is not complete JSON");
    CHECK(app::ReviewRows(nlohmann::json::object()).empty());
    CHECK_FALSE(app::ReplacedNarration(nlohmann::json::object()));
    CHECK_THROWS_AS(app::ReviewRows(nlohmann::json::parse(R"({"review": [{"consistent": true}]})")), core::DataError);
}

TEST_CASE("SceneValues reads the scene a turn's retrieval searched with", "[inspect]")
{
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": ["shore", "npc:marta"]})")) ==
          std::vector<std::string>{"shore", "npc:marta"});
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": []})")).empty());
    CHECK(app::SceneValues(nlohmann::json::object()).empty());
    CHECK_THROWS_AS(app::SceneValues(nlohmann::json::parse(R"({"scene": "shore"})")), core::DataError);
}
