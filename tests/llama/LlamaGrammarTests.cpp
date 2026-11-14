#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
#include "core/World.h"
#include "grammar/Check.h"
#include "grammar/Gbnf.h"
#include "grammar/RandomValue.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The kind of a grammar check's verdict: "fits", "rejects" or "ends early". llama.cpp's reason quotes a token and
// grammar::Check's a code point, so the two agree on the kind, not the quote.
std::string VerdictOf(const std::string& reason)
{
    if (reason.empty()) {
        return "fits";
    }
    if (reason == "the text ends before the grammar does") {
        return "ends early";
    }
    REQUIRE(reason.starts_with("the grammar rejects '"));
    return "rejects";
}

} // namespace

TEST_CASE("llama.cpp accepts the generated grammars and they constrain real output", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const core::World world = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");

    inference::Prompt prompt;
    prompt.messages = {
        {inference::Role::System,
         "You are the game master of a text RPG. Reply with one JSON object: intent, checks, changes.\n\n" +
             context::RenderWorldReference(world)},
        {inference::Role::User, "# Current state\n" + context::RenderStateView(world.initialState) +
                                    "\n# Player\nI climb onto the crawler's roof to scan the horizon.\n\n# Task\n"
                                    "Resolve the action."},
    };
    prompt.stableCount = 1;

    // A grammar llama.cpp cannot parse throws ContractError, so short runs prove each one compiles.
    inference::GenerationSpec probe;
    probe.maxTokens = 4;
    probe.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted);
    CHECK_NOTHROW(backend.Complete(prompt, probe, {}, {}));
    probe.grammar = turn::BuildLookupGrammar();
    CHECK_NOTHROW(backend.Complete(prompt, probe, {}, {}));

    inference::GenerationSpec spec;
    spec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);
    spec.maxTokens = 1024; // Room for a valid but long proposal.
    const inference::Completion completion = backend.Complete(prompt, spec, {}, {});
    // Grammar-constrained decode speed on this machine (recorded in the task report).
    WARN(std::format("resolve: {} tokens in {:.0f} ms ({:.1f} tok/s); output: {}", completion.tokensGenerated,
                     completion.generateMs, completion.tokensGenerated * 1000.0 / std::max(1.0, completion.generateMs),
                     completion.text));
    REQUIRE_FALSE(completion.stoppedAtLimit);
    const nlohmann::json output = nlohmann::json::parse(completion.text);
    CHECK(output.at("intent").is_string());
    CHECK(output.at("checks").is_array());
    CHECK(output.at("changes").is_array());

    // The output is meaningful to the engine: stats and difficulties are the world's, and every change entry has
    // the shape ParseChange reads.
    nlohmann::json allChanges = output.at("changes");
    for (const nlohmann::json& check : output.at("checks")) {
        CHECK(std::ranges::find(world.stats, check.at("stat").get<std::string>()) != world.stats.end());
        CHECK(world.difficulties.contains(check.at("difficulty").get<std::string>()));
        for (const char* branch : {"on_success", "on_failure"}) {
            for (const nlohmann::json& change : check.at(branch)) {
                allChanges.push_back(change);
            }
        }
    }
    for (const nlohmann::json& raw : allChanges) {
        // A change the grammar let through but ParseChange cannot read would be a grammar bug.
        std::string error;
        INFO("change: " << raw.dump());
        REQUIRE(core::ParseChange(raw, error).has_value());
    }
    // Value problems (out-of-range ints, unknown entity ids for creates) are legitimate engine rejections of what
    // the model proposed: shown, not failed.
    const core::ApplyResult applied = core::ApplyChanges(world.schema, world.initialState, allChanges);
    for (const core::RejectedChange& rejected : applied.rejected) {
        WARN(std::format("rejected on apply: {} -- {}", rejected.raw.dump(), rejected.reason));
    }
}

TEST_CASE("LlamaCppBackend::CheckGrammar tells whether a text fits a grammar and where it stops fitting", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::string yesNo = R"(root ::= "{\"answer\":\"" ("yes" | "no") "\"}")";

    // Texts the grammar accepts from start to end.
    CHECK(backend.CheckGrammar(yesNo, R"({"answer":"yes"})").empty());
    CHECK(backend.CheckGrammar(yesNo, R"({"answer":"no"})").empty());

    // A text that breaks the grammar: the reason names the rejected token and the text before it.
    const std::string broken = backend.CheckGrammar(yesNo, R"({"answer":"maybe"})");
    CHECK_THAT(broken, ContainsSubstring("the grammar rejects") && ContainsSubstring("answer"));
    const std::string trailing = backend.CheckGrammar(yesNo, R"({"answer":"yes"} and more)");
    CHECK_THAT(trailing, ContainsSubstring("the grammar rejects") && ContainsSubstring("yes"));

    // A text that ends early.
    CHECK(backend.CheckGrammar(yesNo, R"({"answer":"ye)") == "the text ends before the grammar does");
    CHECK(backend.CheckGrammar(yesNo, "") == "the text ends before the grammar does");

    // A grammar that does not parse, or none at all, is an engine bug.
    CHECK_THROWS_AS(backend.CheckGrammar("root ::= (", "x"), core::ContractError);
    CHECK_THROWS_AS(backend.CheckGrammar("", "x"), core::ContractError);
}

TEST_CASE("llama.cpp loads FromSchema grammars at the repetition limits", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // CheckGrammar throws ContractError for a grammar llama.cpp refuses to parse; "" means it parsed and `value` fits.
    auto loads = [&](const nlohmann::json& schema, const std::string& value) {
        INFO("schema: " << schema.dump());
        CHECK(backend.CheckGrammar(grammar::FromSchema(schema), value).empty());
    };

    // The longest string, full.
    const nlohmann::json longString = {{"type", "string"}, {"maxLength", grammar::GBNF_MAX_STRING_CHARS}};
    loads(longString, "\"" + std::string(grammar::GBNF_MAX_STRING_CHARS, 'a') + "\"");
    // The longest list of the longest strings, full.
    const nlohmann::json longList = {{"type", "array"}, {"items", longString}, {"maxItems", grammar::GBNF_MAX_LIST_ITEMS}};
    std::string fullList = "[";
    for (int i = 0; i < grammar::GBNF_MAX_LIST_ITEMS; ++i) {
        fullList += i == 0 ? R"("a")" : R"(,"a")";
    }
    loads(longList, fullList + "]");
    // The longest list of objects that each hold the longest list.
    const nlohmann::json holder = {
        {"type", "object"}, {"properties", {{"tags", longList}}}, {"required", nlohmann::json::array({"tags"})}};
    loads({{"type", "array"}, {"items", holder}, {"maxItems", grammar::GBNF_MAX_LIST_ITEMS}},
          R"([{"tags":["a","b"]},{"tags":[]}])");
    // The host's catalog, and a list of short strings whose items FromSchema names as a rule.
    loads(test::CatalogSchema(), R"({"action":"give_item","params":{"item":"Iron Sword"}})");
    loads(nlohmann::json::parse(R"({"type": "array", "items": {"type": "string", "maxLength": 48}, "maxItems": 50})"),
          R"(["Iron Sword","Bread"])");

    // What llama.cpp refuses, written by hand: one character more than the limit, and that same list of strings with
    // the string written inline in the repeated group (49 symbols repeated 49 times), as FromSchema wrote it before.
    const std::string tooLong =
        grammar::GrammarBuilder().Text(grammar::Literal("\"") + " char{0,2000} " + grammar::Literal("\""));
    CHECK_THROWS_AS(backend.CheckGrammar(tooLong, R"("a")"), core::ContractError);
    const std::string item = grammar::JsonString(0, 48);
    const std::string inlineList = grammar::GrammarBuilder().Text(grammar::Literal("[") + " " + item + " (" +
                                                                  grammar::Literal(",") + " " + item + "){0,49} " +
                                                                  grammar::Literal("]"));
    CHECK_THROWS_AS(backend.CheckGrammar(inlineList, R"(["Iron Sword"])"), core::ContractError);
}

TEST_CASE("llama.cpp and grammar::Check accept the random values of each test schema", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<nlohmann::json> schemas = {test::CatalogSchema(), test::EveryKeywordSchema()};
    for (const nlohmann::json& schema : schemas) {
        const std::string gbnf = grammar::FromSchema(schema);
        // The grammar parses (one that does not is a ContractError) and wants a whole value, by both checkers.
        CHECK(backend.CheckGrammar(gbnf, "") == "the text ends before the grammar does");
        CHECK(grammar::Check(gbnf, "") == "the text ends before the grammar does");
        // The same values as the fast suite's property test: the same seed, the same draws, written compact.
        std::mt19937_64 random(test::PROPERTY_SEED);
        for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
            const std::string text = test::RandomValue(schema, random).dump();
            INFO("value " << i << ": " << text);
            CHECK(backend.CheckGrammar(gbnf, text) == "");
            CHECK(grammar::Check(gbnf, text) == "");
        }
    }
}

TEST_CASE("llama.cpp rejects invalid actions against the catalog grammar", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::string gbnf = grammar::FromSchema(test::CatalogSchema());
    const std::vector<std::string> invalid = {
        R"({"action":"dance","params":{}})",                                             // an unknown action
        R"({"action":"attack","params":{"target":"0001C3D2"}})",                         // an actor who is not present
        R"({"action":"attack","params":{}})",                                            // a missing argument
        R"({"action":"wait","params":{},"intent":"rest"})",                              // an extra key
        R"({"action":"give_item","params":{"item":")" + std::string(49, 'a') + R"("}})", // an item over 48 characters
        R"({"action": "wait","params":{}})",                                             // whitespace
    };
    for (const std::string& text : invalid) {
        INFO(text);
        const std::string reason = backend.CheckGrammar(gbnf, text);
        CHECK(reason != "");
        CHECK(VerdictOf(grammar::Check(gbnf, text)) == VerdictOf(reason));
    }
}

TEST_CASE("llama.cpp parses the review grammars built on the builder", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // A grammar llama.cpp cannot parse is a ContractError, so each check also proves the grammar compiles.
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true),
                               R"({"consistent":false,"problems":["The roll failed."],"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, false), R"({"consistent":true,"problems":[]})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(false, true), R"({"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true), R"({"consistent":true,"problems":["x"],"digest":""})") != "");
}

TEST_CASE("llama.cpp and grammar::Check agree on IntRange grammars", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<std::pair<long long, long long>> ranges = {{-15, 123}, {0, 255}, {-1000, -91}, {37, 120}};
    for (const auto& [low, high] : ranges) {
        const std::string gbnf = grammar::GrammarBuilder().Text(grammar::IntRange(low, high));
        // Other spellings, then a window around each end.
        std::vector<std::pair<std::string, bool>> probes = {{"-0", false}, {"00", false}, {"007", false}, {"-01", false},
                                                            {"+5", false}};
        for (long long value = low - 12; value <= low + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (long long value = high - 12; value <= high + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (const auto& [text, inRange] : probes) {
            INFO("IntRange(" << low << ", " << high << ") on '" << text << "'");
            const std::string llamaReason = backend.CheckGrammar(gbnf, text);
            const std::string checkReason = grammar::Check(gbnf, text);
            CHECK(llamaReason.empty() == inRange);
            CHECK(checkReason.empty() == inRange);
            CHECK(VerdictOf(checkReason) == VerdictOf(llamaReason));
        }
    }
}
