#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "grammar/GbnfMatcher.h"
#include "core/Errors.h"
#include "core/StateSchema.h"
#include "core/World.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

// The mini world with its schema and state replaced, so a test can use any field mix.
core::World WorldWith(const nlohmann::json& schema, const nlohmann::json& state)
{
    core::World world = MiniWorld();
    world.schema = core::StateSchema::FromJson(schema);
    world.initialState = state;
    return world;
}

// A schema with every field type: a character with all five, and a creatable npc with an enum.
nlohmann::json AllFieldsSchema()
{
    return nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {
                "creatable": true,
                "fields": {"disposition": {"type": "enum", "values": ["wary", "friendly"]}}
            }
        }
    })");
}

nlohmann::json PlayerState(const nlohmann::json& inventory)
{
    return {{"player",
             {{"type", "character"},
              {"fields",
               {{"health", 10}, {"location", "shore"}, {"conditions", nlohmann::json::array()}, {"inventory", inventory}}}}}};
}

// The name of the rule "<prefix>-N ::= <rhs>" in the grammar, found by its right-hand side. Empty when absent.
std::string FindRuleName(const std::string& grammar, const std::string& prefix, const std::string& rhs)
{
    std::istringstream lines(grammar);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t arrow = line.find(" ::= ");
        if (arrow != std::string::npos && line.compare(0, prefix.size() + 1, prefix + "-") == 0 &&
            line.substr(arrow + 5) == rhs) {
            return line.substr(0, arrow);
        }
    }
    return "";
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// A compact JSON array of `count` copies of the JSON text `item`.
std::string JsonList(const std::string& item, int count)
{
    std::string items;
    for (int i = 0; i < count; ++i) {
        items += (i == 0 ? "" : ",") + item;
    }
    return "[" + items + "]";
}

// Checks the GBNF is well formed: every line is "name ::= rhs", every rule is defined exactly once, every rule
// name used outside quotes and character classes is defined, and parentheses, quotes and brackets balance.
// Returns "" when it is, otherwise the first problem.
std::string GrammarProblem(const std::string& grammar)
{
    std::map<std::string, int> defined;
    std::map<std::string, std::string> used; // Referenced name -> the rule that references it.
    std::istringstream lines(grammar);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t arrow = line.find(" ::= ");
        if (arrow == std::string::npos) {
            return "line without ' ::= ': " + line;
        }
        const std::string name = line.substr(0, arrow);
        ++defined[name];
        const std::string rhs = line.substr(arrow + 5);
        int depth = 0;
        for (std::size_t i = 0; i < rhs.size(); ++i) {
            const char c = rhs[i];
            if (c == '"' || c == '[') {
                // A string literal or a character class: skip to its closer, honouring backslash escapes.
                const char closer = c == '"' ? '"' : ']';
                std::size_t j = i + 1;
                while (j < rhs.size() && rhs[j] != closer) {
                    j += rhs[j] == '\\' ? 2 : 1;
                }
                if (j >= rhs.size()) {
                    return std::string(c == '"' ? "unbalanced quote in " : "unbalanced bracket in ") + name;
                }
                i = j;
            } else if (c == '{') {
                // A repetition count such as {0,5}.
                const std::size_t close = rhs.find('}', i);
                if (close == std::string::npos) {
                    return "unbalanced brace in " + name;
                }
                i = close;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (--depth < 0) {
                    return "unbalanced parenthesis in " + name;
                }
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
                std::size_t end = i;
                while (end < rhs.size() &&
                       (std::isalnum(static_cast<unsigned char>(rhs[end])) || rhs[end] == '-' || rhs[end] == '_')) {
                    ++end;
                }
                used.emplace(rhs.substr(i, end - i), name);
                i = end - 1;
            }
        }
        if (depth != 0) {
            return "unbalanced parenthesis in " + name;
        }
    }
    for (const auto& [name, count] : defined) {
        if (count != 1) {
            return "rule '" + name + "' is defined " + std::to_string(count) + " times";
        }
    }
    if (!defined.contains("root")) {
        return "no root rule";
    }
    for (const auto& [name, user] : used) {
        if (!defined.contains(name)) {
            return "rule '" + name + "' used by '" + user + "' is not defined";
        }
    }
    return "";
}

} // namespace

TEST_CASE("The grammar checker finds broken grammars", "[grammar]")
{
    CHECK(GrammarProblem("root ::= \"a\" | b\nb ::= [x]{1,2}\n").empty());
    CHECK_THAT(GrammarProblem("root ::= missing\n"), ContainsSubstring("not defined"));
    CHECK_THAT(GrammarProblem("root ::= \"a\"\nroot ::= \"b\"\n"), ContainsSubstring("defined 2 times"));
    CHECK_THAT(GrammarProblem("root ::= (\"a\"\n"), ContainsSubstring("parenthesis"));
    CHECK_THAT(GrammarProblem("root ::= \"a\n"), ContainsSubstring("quote"));
}

TEST_CASE("The resolve grammar names the world's stats, difficulties and field shapes", "[grammar]")
{
    const core::World world = MiniWorld();
    const std::string grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine);

    CHECK_THAT(grammar, ContainsSubstring(R"(root ::= "{\"intent\":" short-text ",\"checks\":" checks ",\"changes\":" changes "}")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(stat ::= "\"agility\"")"));
    // Exact text from the grammar builder: an enum's alternatives come in parentheses.
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= ("\"easy\"" | "\"hard\""))"));
    // An int field: set or add an integer.
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":" ("\"set\"" | "\"add\"") ",\"path\":\"player.health\",\"value\":" int reason)"));
    // A string field: set only.
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"set\"" ",\"path\":\"player.location\",\"value\":" short-text reason)"));
    // A counter field: add a non-zero count to an item, or remove the item. The player holds a lantern, so the item
    // part is a per-field rule that matches it by name or any new item.
    const std::string items = FindRuleName(grammar, "items", R"(("lantern" | item))"); // The builder's parentheses.
    REQUIRE_FALSE(items.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"add\",\"path\":\"player.inventory." )" + items + R"( "\",\"value\":" nonzero-int)"));
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"remove\",\"path\":\"player.inventory." )" + items + R"( "\"")"));
    // Create: a new id and every field of the creatable type.
    const std::string disposition = FindRuleName(grammar, "enum", R"(("\"wary\"" | "\"friendly\""))"); // Builder text.
    REQUIRE_FALSE(disposition.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"(",\"value\":{\"type\":\"npc\",\"fields\":{" "\"disposition\":" )" + disposition + R"( "}}" reason)"));
    CHECK(GrammarProblem(grammar).empty());
    CHECK_THAT(grammar, !ContainsSubstring("notes"));
}

TEST_CASE("The resolve grammar follows the live state and the memory mode", "[grammar]")
{
    const core::World world = MiniWorld();
    nlohmann::json state = world.initialState;
    state["npc:marta"] = {{"type", "npc"}, {"fields", {{"disposition", "wary"}}}};

    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Assisted);
    CHECK_THAT(grammar, ContainsSubstring(R"(\"npc:marta.disposition\")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(",\"notes\":" notes "}")"));
    CHECK_THAT(grammar, ContainsSubstring("notes ::= "));
    CHECK(GrammarProblem(grammar).empty());

    // The live npc's enum field and a create of the same type share one enum rule.
    const std::string disposition = FindRuleName(grammar, "enum", R"(("\"wary\"" | "\"friendly\""))"); // Builder text.
    REQUIRE_FALSE(disposition.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"set\"" ",\"path\":\"npc:marta.disposition\",\"value\":" )" + disposition + " reason"));
    CHECK_THAT(grammar, ContainsSubstring(R"("\"disposition\":" )" + disposition + R"( "}}" reason)"));
    CHECK(CountOccurrences(grammar, R"(::= ("\"wary\"")") == 1); // The builder's parentheses.
}

TEST_CASE("The resolve grammar bounds every list", "[grammar]")
{
    const core::World world = MiniWorld();
    const std::string grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted);
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    // Exact text from the grammar builder: a list that may be empty is a group of two alternatives.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= ("[]" | "[" check ("," check){0,2} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= ("[]" | "[" change ("," change){0,5} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= ("[]" | "[" change ("," change){0,3} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= ("[]" | "[" long-text ("," long-text){0,1} "]"))"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= ("[]" | "[" lookup ("," lookup){0,2} "]"))"));
}

TEST_CASE("The resolve grammar covers list fields and enum sets on existing entities", "[grammar]")
{
    nlohmann::json state = PlayerState(nlohmann::json::object());
    state["npc:marta"] = {{"type", "npc"}, {"fields", {{"disposition", "wary"}}}};
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine);

    // A list field: add or remove one string.
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":" ("\"add\"" | "\"remove\"") ",\"path\":\"player.conditions\",\"value\":" short-text reason)"));
    // An enum field of an existing entity: set one of its values.
    const std::string disposition = FindRuleName(grammar, "enum", R"(("\"wary\"" | "\"friendly\""))"); // Builder text.
    REQUIRE_FALSE(disposition.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"set\"" ",\"path\":\"npc:marta.disposition\",\"value\":" )" + disposition + " reason"));
    CHECK(GrammarProblem(grammar).empty());
}

TEST_CASE("The resolve grammar escapes enum values", "[grammar]")
{
    // The value a"b\c is the JSON string "a\"b\\c", written once more as a GBNF literal.
    const nlohmann::json schema = nlohmann::json::parse(R"({
        "entity_types": {"npc": {"fields": {"mood": {"type": "enum", "values": ["a\"b\\c", "plain"]}}}}
    })");
    const nlohmann::json state = {{"npc:x", {{"type", "npc"}, {"fields", {{"mood", "plain"}}}}}};
    const core::World world = WorldWith(schema, state);
    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine);

    CHECK_THAT(grammar, ContainsSubstring(R"(::= ("\"a\\\"b\\\\c\"" | "\"plain\""))")); // The builder's parentheses.
    CHECK(GrammarProblem(grammar).empty());
}

TEST_CASE("Held counter items are matched by name next to the generic item rule", "[grammar]")
{
    // "Rusty Knife" and "water-flask" cannot be spelled by [a-z0-9_]; the quoted name needs escaping twice.
    const nlohmann::json state = PlayerState({{"Rusty Knife", 1}, {"water-flask", 2}, {"say \"hi\"", 1}});
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine);

    // The rule lists the held names (a std::map order: capital letters first) and still allows a new item.
    // Exact text from the grammar builder: the alternatives come in parentheses.
    const std::string items =
        FindRuleName(grammar, "items", R"(("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item))");
    REQUIRE_FALSE(items.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"add\",\"path\":\"player.inventory." )" + items + R"( "\",\"value\":" nonzero-int)"));
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"remove\",\"path\":\"player.inventory." )" + items + R"( "\"")"));
    CHECK(GrammarProblem(grammar).empty());

    // With nothing held, the item part is the generic rule itself.
    const nlohmann::json emptyState = PlayerState(nlohmann::json::object());
    const std::string emptyGrammar = turn::BuildResolveGrammar(WorldWith(AllFieldsSchema(), emptyState), emptyState,
                                                               core::MemoryMode::Engine);
    CHECK_THAT(emptyGrammar, ContainsSubstring(R"(player.inventory." item "\",\"value\":" nonzero-int)"));
    CHECK_THAT(emptyGrammar, !ContainsSubstring("items-"));
}

TEST_CASE("Every grammar is well formed", "[grammar]")
{
    const core::World world = MiniWorld();
    nlohmann::json state = world.initialState;
    state["npc:marta"] = {{"type", "npc"}, {"fields", {{"disposition", "wary"}}}};

    CHECK(GrammarProblem(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine)).empty());
    CHECK(GrammarProblem(turn::BuildResolveGrammar(world, state, core::MemoryMode::Assisted)).empty());
    CHECK(GrammarProblem(turn::BuildLookupGrammar()).empty());
}

TEST_CASE("A state with nothing to change gets a grammar whose change lists are empty", "[grammar]")
{
    // No entity exists and the only type is not creatable, so no change can be written. state_schema.json cannot
    // declare a type without fields, so an empty state is how a world reaches this.
    const nlohmann::json schema = nlohmann::json::parse(R"({
        "entity_types": {"character": {"fields": {"health": {"type": "int", "min": 0, "max": 10}}}}
    })");
    const nlohmann::json state = nlohmann::json::object();
    const core::World world = WorldWith(schema, state);
    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine);

    CHECK_THAT(grammar, ContainsSubstring("\nchanges ::= \"[]\"\n"));
    CHECK_THAT(grammar, ContainsSubstring("\nbranch ::= \"[]\"\n"));
    CHECK_THAT(grammar, !ContainsSubstring("\nchange ::="));
    CHECK_THAT(grammar, !ContainsSubstring("change-"));
    CHECK(GrammarProblem(grammar).empty());
}

TEST_CASE("The resolve grammar refuses state that is not an object", "[grammar]")
{
    const core::World world = MiniWorld();
    CHECK_THROWS_AS(turn::BuildResolveGrammar(world, nlohmann::json::array(), core::MemoryMode::Engine), core::ContractError);
    CHECK_THROWS_AS(turn::BuildResolveGrammar(world, nullptr, core::MemoryMode::Engine), core::ContractError);
}

TEST_CASE("The lookup grammar lists sources and queries", "[grammar]")
{
    const std::string grammar = turn::BuildLookupGrammar();
    CHECK_THAT(grammar, ContainsSubstring(R"(root ::= "{\"lookups\":" lookups "}")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(("\"lore\"" | "\"history\"" | "\"notes\""))"));
}

TEST_CASE("The review grammar asks for the verdict, the digest or both", "[grammar]")
{
    // Both: consistent with no problems, or inconsistent with one to three of them; the digest follows the verdict.
    const std::string both = turn::BuildReviewGrammar(true, true);
    CHECK(both.starts_with("root ::= review\n"));
    // Exact text from the grammar builder: two objects as alternatives, each key and value its own fragment.
    CHECK_THAT(both, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" ",\"digest\":" digest-text "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}"))"));
    CHECK_THAT(both, ContainsSubstring(R"(problem-text ::= "\"" char{1,200} "\"")"));
    CHECK_THAT(both, ContainsSubstring(R"(digest-text ::= "\"" char{0,300} "\"")"));
    CHECK(GrammarProblem(both).empty());

    // The check alone: no digest.
    const std::string check = turn::BuildReviewGrammar(true, false);
    CHECK(check.starts_with("root ::= review\n"));
    CHECK_THAT(check, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" "}"))"));
    CHECK_THAT(check, !ContainsSubstring("digest"));
    CHECK(GrammarProblem(check).empty());

    // The digest alone: no verdict.
    const std::string digest = turn::BuildReviewGrammar(false, true);
    CHECK(digest.starts_with("root ::= review\n"));
    CHECK_THAT(digest, ContainsSubstring(R"(review ::= "{\"digest\":" digest-text "}")" "\n"));
    CHECK_THAT(digest, !ContainsSubstring("consistent"));
    CHECK(GrammarProblem(digest).empty());

    // A review that asks for nothing is an engine bug.
    CHECK_THROWS_AS(turn::BuildReviewGrammar(false, false), core::ContractError);
}

// Characterization: what the grammars accept and refuse, checked with the test matcher. The builder rewrite keeps
// every one of these, so they show the rewritten grammars accept the same language.

TEST_CASE("The resolve grammar accepts the proposals it describes and refuses the rest", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher engine(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));

    const std::vector<std::string> accepted = {
        R"({"intent":"look","checks":[],"changes":[]})",
        R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall","on_success":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs up"}],"on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],"changes":[]})",
        R"({"intent":"light","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":-1,"reason":"burns oil"},{"op":"remove","path":"player.inventory.lantern","reason":"set in place"},{"op":"add","path":"player.inventory.lantern","value":999999999,"reason":"r"}]})",
        R"({"intent":"meet","checks":[],"changes":[{"op":"create","path":"npc:marta","value":{"type":"npc","fields":{"disposition":"wary"}},"reason":"she arrives"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":999999999,"reason":"r"},{"op":"set","path":"player.agility","value":-999999999,"reason":"r"},{"op":"add","path":"player.health","value":0,"reason":"r"}]})",
        R"({"intent":"caf\u00e9 \"quoted\"\n","checks":[],"changes":[]})",
        "{\"intent\":\"caf\xC3\xA9 \xE6\x97\xA5\",\"checks\":[],\"changes\":[]}",
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(engine.Accepts(text));
    }

    const std::vector<std::string> rejected = {
        // Integers: no "-0", no leading zero, at most 9 digits; a counter add is never zero.
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":-0,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":05,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":1000000000,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":-1000000000,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":0,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":-0,"reason":"r"}]})",
        // The world's stats and difficulties only.
        R"({"intent":"x","checks":[{"stat":"strength","difficulty":"hard","stakes":"s","on_success":[],"on_failure":[]}],"changes":[]})",
        R"({"intent":"x","checks":[{"stat":"agility","difficulty":"medium","stakes":"s","on_success":[],"on_failure":[]}],"changes":[]})",
        // Compact, in order, and notes only in assisted mode.
        R"({"intent": "look","checks":[],"changes":[]})",
        R"({"intent":"look","changes":[],"checks":[]})",
        R"({"intent":"look","checks":[],"changes":[],"notes":[]})",
        // Only the shapes the schema allows.
        R"({"intent":"x","checks":[],"changes":[{"op":"remove","path":"player.inventory.lantern","value":1,"reason":"r"}]})",
        // An existing-entity change needs the entity to exist now: npc:marta is only creatable, not in the state.
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"npc:marta.disposition","value":"wary","reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.Oil","value":1,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.location","value":5,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.location","value":"x","reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.location","value":"x"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"create","path":"npc:marta","value":{"type":"npc","fields":{"disposition":"angry"}},"reason":"r"}]})",
        // Text: no raw control character, no raw DEL.
        "{\"intent\":\"a\nb\",\"checks\":[],\"changes\":[]}",
        "{\"intent\":\"a\x7F\",\"checks\":[],\"changes\":[]}",
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(engine.Accepts(text));
    }
}

TEST_CASE("The resolve grammar bounds its lists and texts exactly", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher engine(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));
    const std::string change = R"({"op":"set","path":"player.location","value":"x","reason":"r"})";
    // A check whose success branch holds `branchChanges` changes.
    auto checkWith = [&](int branchChanges) {
        return R"({"stat":"agility","difficulty":"easy","stakes":"s","on_success":)" + JsonList(change, branchChanges) +
               R"(,"on_failure":[]})";
    };
    auto proposal = [&](const std::string& intent, int checks, int branchChanges, int changes) {
        return R"({"intent":")" + intent + R"(","checks":)" + JsonList(checkWith(branchChanges), checks) +
               R"(,"changes":)" + JsonList(change, changes) + "}";
    };
    const std::size_t shortText = static_cast<std::size_t>(turn::SHORT_TEXT_CHARS);

    CHECK(engine.Accepts(proposal(std::string(shortText, 'a'), turn::MAX_CHECKS, turn::MAX_BRANCH_CHANGES,
                                  turn::MAX_CHANGES)));
    CHECK_FALSE(engine.Accepts(proposal(std::string(shortText + 1, 'a'), 0, 0, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", turn::MAX_CHECKS + 1, 0, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", 1, turn::MAX_BRANCH_CHANGES + 1, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", 0, 0, turn::MAX_CHANGES + 1)));
}

TEST_CASE("The assisted resolve grammar and the lookup grammar accept exactly their shapes", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher assisted(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted));
    const std::string note(static_cast<std::size_t>(turn::LONG_TEXT_CHARS), 'n');
    CHECK(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":[]})"));
    CHECK(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":["The keeper hums.",")" + note + R"("]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":["a","b","c"]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":[")" + note + R"(n"]})"));

    const test::GbnfMatcher lookup(turn::BuildLookupGrammar());
    CHECK(lookup.Accepts(R"({"lookups":[]})"));
    CHECK(lookup.Accepts(R"({"lookups":[{"source":"lore","query":"keeper"},{"source":"history","query":""},{"source":"notes","query":"the shanty"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"lore","query":"a"},{"source":"lore","query":"b"},{"source":"lore","query":"c"},{"source":"lore","query":"d"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"web","query":"x"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"query":"x","source":"lore"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"lore"}]})"));
}

TEST_CASE("The resolve grammar's creates accept exactly the field values of their type", "[grammar]")
{
    const nlohmann::json schema = nlohmann::json::parse(R"({
        "entity_types": {
            "pack": {
                "creatable": true,
                "fields": {
                    "contents": {"type": "counter"},
                    "kind": {"type": "enum", "values": ["small", "large"]},
                    "label": {"type": "string"},
                    "size": {"type": "int", "min": 0, "max": 9},
                    "tags": {"type": "list"}
                }
            }
        }
    })");
    const nlohmann::json state = nlohmann::json::object();
    const core::World world = WorldWith(schema, state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    // A create of a pack with the id `id` and the fields object `fields`.
    auto create = [](const std::string& id, const std::string& fields) {
        return R"({"intent":"pack","checks":[],"changes":[{"op":"create","path":")" + id +
               R"(","value":{"type":"pack","fields":)" + fields + R"(},"reason":"r"}]})";
    };
    // A counter value with `count` new items, i0 to i<count - 1>, of one each.
    auto contents = [](int count) {
        std::string items;
        for (int i = 0; i < count; ++i) {
            items += std::format(R"({}"i{}":1)", i == 0 ? "" : ",", i);
        }
        return "{" + items + "}";
    };
    const std::string rest = R"("kind":"small","label":"bag","size":0,"tags":[])";
    const std::size_t idChars = static_cast<std::size_t>(turn::MAX_NEW_ID_CHARS);

    CHECK(matcher.Accepts(create("pack:1", R"({"contents":{},)" + rest + "}")));
    CHECK(matcher.Accepts(create("pack_2", R"({"contents":{"rope":2,"oil":999999999},"kind":"large","label":"","size":-3,"tags":["a","b"]})")));
    CHECK(matcher.Accepts(create(std::string(idChars, 'p'), R"({"contents":{},)" + rest + "}")));
    CHECK(matcher.Accepts(create("pack:1", R"({"contents":)" + contents(turn::MAX_LIST_ITEMS) + "," + rest + "}")));
    CHECK(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0,"tags":)" +
                                               JsonList(R"("t")", turn::MAX_LIST_ITEMS) + "}")));

    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":)" + contents(turn::MAX_LIST_ITEMS + 1) + "," + rest + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0,"tags":)" +
                                                     JsonList(R"("t")", turn::MAX_LIST_ITEMS + 1) + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{"rope":0},)" + rest + "}"))); // counts are positive
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{"Rope":1},)" + rest + "}"))); // new items: [a-z0-9_]
    CHECK_FALSE(matcher.Accepts(create("Pack:1", R"({"contents":{},)" + rest + "}")));         // ids: [a-z0-9_:]
    CHECK_FALSE(matcher.Accepts(create(std::string(idChars + 1, 'p'), R"({"contents":{},)" + rest + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0})")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"medium","label":"bag","size":0,"tags":[]})")));
}

TEST_CASE("The resolve grammar accepts held counter items by name and list values", "[grammar]")
{
    const nlohmann::json state = PlayerState({{"Rusty Knife", 1}, {"say \"hi\"", 1}});
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    auto proposal = [](const std::string& change) {
        return R"({"intent":"x","checks":[],"changes":[)" + change + "]}";
    };
    CHECK(matcher.Accepts(proposal(R"({"op":"remove","path":"player.inventory.Rusty Knife","reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"add","path":"player.inventory.say \"hi\"","value":2,"reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"add","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"remove","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"remove","path":"player.inventory.Rusty Spoon","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"set","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"add","path":"player.inventory.say "hi"","value":2,"reason":"r"})")));
}

TEST_CASE("The review grammars accept exactly the verdicts and digests asked for", "[grammar]")
{
    const std::string digest300(static_cast<std::size_t>(turn::DIGEST_MAX_CHARS), 'd');
    const std::string problem200(static_cast<std::size_t>(turn::REVIEW_PROBLEM_MAX_CHARS), 'p');

    const test::GbnfMatcher both(turn::BuildReviewGrammar(true, true));
    CHECK(both.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));
    CHECK(both.Accepts(R"({"consistent":false,"problems":["p","q","r"],"digest":""})"));
    CHECK(both.Accepts(R"({"consistent":true,"problems":[],"digest":")" + digest300 + R"("})"));
    CHECK(both.Accepts(R"({"consistent":false,"problems":[")" + problem200 + R"("],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":["p"],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":["p","q","r","s"],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[""],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":[]})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":[],"digest":")" + digest300 + R"(d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[")" + problem200 + R"(p"],"digest":"d"})"));

    const test::GbnfMatcher check(turn::BuildReviewGrammar(true, false));
    CHECK(check.Accepts(R"({"consistent":true,"problems":[]})"));
    CHECK(check.Accepts(R"({"consistent":false,"problems":["p"]})"));
    CHECK_FALSE(check.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));

    const test::GbnfMatcher digest(turn::BuildReviewGrammar(false, true));
    CHECK(digest.Accepts(R"({"digest":"d"})"));
    CHECK_FALSE(digest.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));
}

// Fix round: the rules the first characterization left unpinned.

namespace {

// A proposal with no checks and the given changes (JSON texts).
std::string ProposalOf(const std::string& changes)
{
    return R"({"intent":"x","checks":[],"changes":[)" + changes + "]}";
}

// `count` repetitions of `unit`.
std::string Repeat(const std::string& unit, int count)
{
    std::string result;
    for (int i = 0; i < count; ++i) {
        result += unit;
    }
    return result;
}

} // namespace

TEST_CASE("The resolve grammar changes an enum on an existing entity only with set and a listed value", "[grammar]")
{
    nlohmann::json state = PlayerState(nlohmann::json::object());
    state["npc:marta"] = {{"type", "npc"}, {"fields", {{"disposition", "wary"}}}};
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    auto change = [](const std::string& op, const std::string& value) {
        return ProposalOf(R"({"op":")" + op + R"(","path":"npc:marta.disposition")" +
                          (value.empty() ? "" : R"(,"value":)" + value) + R"(,"reason":"r"})");
    };
    for (const std::string& text : {change("set", R"("wary")"), change("set", R"("friendly")")}) {
        INFO(text);
        CHECK(matcher.Accepts(text));
    }
    for (const std::string& text : {change("set", R"("angry")"), change("set", R"("Wary")"), change("set", "1"),
                                    change("add", R"("wary")"), change("remove", R"("wary")"), change("remove", ""),
                                    change("set", "")}) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }
}

TEST_CASE("The resolve grammar's integers have exact edges", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));
    auto add = [](const std::string& value) {
        return ProposalOf(R"({"op":"add","path":"player.inventory.oil","value":)" + value + R"(,"reason":"r"})");
    };
    for (const std::string& value : {"-999999999", "-1", "1", "999999999"}) {
        INFO(value);
        CHECK(matcher.Accepts(add(value)));
    }
    for (const std::string& value : {"-1000000000", "1000000000", "0", "-0", "05", "-05", "+1", "1.0", "1e2", ""}) {
        INFO(value);
        CHECK_FALSE(matcher.Accepts(add(value)));
    }

    // A counter value inside a create takes positive integers only.
    const nlohmann::json schema = nlohmann::json::parse(R"({"entity_types": {"pack": {"creatable": true,
        "fields": {"contents": {"type": "counter"}}}}})");
    const nlohmann::json state = nlohmann::json::object();
    const test::GbnfMatcher creates(
        turn::BuildResolveGrammar(WorldWith(schema, state), state, core::MemoryMode::Engine));
    auto create = [](const std::string& contents) {
        return ProposalOf(R"({"op":"create","path":"pack:1","value":{"type":"pack","fields":{"contents":)" + contents +
                          R"(}},"reason":"r"})");
    };
    for (const std::string& count : {"1", "999999999"}) {
        INFO(count);
        CHECK(creates.Accepts(create(R"({"rope":)" + count + "}")));
    }
    for (const std::string& count : {"0", "-1", "1000000000", "05"}) {
        INFO(count);
        CHECK_FALSE(creates.Accepts(create(R"({"rope":)" + count + "}")));
    }
}

TEST_CASE("The resolve grammar's counter items are bounded and lowercase", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));
    auto add = [](const std::string& item) {
        return ProposalOf(R"({"op":"add","path":"player.inventory.)" + item + R"(","value":1,"reason":"r"})");
    };
    const std::size_t maxItem = static_cast<std::size_t>(turn::MAX_ITEM_CHARS);
    for (const std::string& item : {std::string(maxItem, 'a'), std::string("a_9"), std::string("z")}) {
        INFO(item);
        CHECK(matcher.Accepts(add(item)));
    }
    for (const std::string& item : {std::string(maxItem + 1, 'a'), std::string(""), std::string("two words"),
                                    std::string("water-flask"), std::string("Oil"), std::string("oiL")}) {
        INFO(item);
        CHECK_FALSE(matcher.Accepts(add(item)));
    }

    // The same bounds on the new items of a counter value inside a create.
    const nlohmann::json schema = nlohmann::json::parse(R"({"entity_types": {"pack": {"creatable": true,
        "fields": {"contents": {"type": "counter"}}}}})");
    const nlohmann::json state = nlohmann::json::object();
    const test::GbnfMatcher creates(
        turn::BuildResolveGrammar(WorldWith(schema, state), state, core::MemoryMode::Engine));
    auto create = [](const std::string& item) {
        return ProposalOf(R"({"op":"create","path":"pack:1","value":{"type":"pack","fields":{"contents":{")" + item +
                          R"(":1}}},"reason":"r"})");
    };
    CHECK(creates.Accepts(create(std::string(maxItem, 'a'))));
    for (const std::string& item : {std::string(maxItem + 1, 'a'), std::string(""), std::string("two words"),
                                    std::string("water-flask"), std::string("Oil")}) {
        INFO(item);
        CHECK_FALSE(creates.Accepts(create(item)));
    }
}

TEST_CASE("The resolve grammar matches held names whole and handles worlds with little to change", "[grammar]")
{
    const nlohmann::json heldState = PlayerState({{"Rusty Knife", 1}});
    const test::GbnfMatcher held(turn::BuildResolveGrammar(WorldWith(AllFieldsSchema(), heldState), heldState,
                                                           core::MemoryMode::Engine));
    auto remove = [](const std::string& item) {
        return ProposalOf(R"({"op":"remove","path":"player.inventory.)" + item + R"(","reason":"r"})");
    };
    CHECK(held.Accepts(remove("Rusty Knife")));
    for (const std::string& item : {"Rusty Kni", "Rusty Knife2", "Rusty", "rusty knife", "Rusty Knife "}) {
        INFO(item);
        CHECK_FALSE(held.Accepts(remove(item)));
    }

    // An empty counter has just the generic new-item rule.
    const nlohmann::json emptyState = PlayerState(nlohmann::json::object());
    const test::GbnfMatcher empty(turn::BuildResolveGrammar(WorldWith(AllFieldsSchema(), emptyState), emptyState,
                                                            core::MemoryMode::Engine));
    CHECK(empty.Accepts(remove("new_item")));
    CHECK_FALSE(empty.Accepts(remove("Rusty Knife")));
    CHECK_FALSE(empty.Accepts(remove("")));

    // Nothing changeable: no entity, no creatable type. Every change list is empty.
    const nlohmann::json schema = nlohmann::json::parse(R"({"entity_types": {"pack": {
        "fields": {"label": {"type": "string"}}}}})");
    const nlohmann::json state = nlohmann::json::object();
    const test::GbnfMatcher none(turn::BuildResolveGrammar(WorldWith(schema, state), state, core::MemoryMode::Engine));
    const std::string checkText =
        R"({"stat":"agility","difficulty":"easy","stakes":"s","on_success":[],"on_failure":[]})";
    CHECK(none.Accepts(R"({"intent":"x","checks":[],"changes":[]})"));
    CHECK(none.Accepts(R"({"intent":"x","checks":[)" + checkText + R"(],"changes":[]})"));
    CHECK_FALSE(none.Accepts(ProposalOf(R"({"op":"set","path":"pack:1.label","value":"a","reason":"r"})")));
    CHECK_FALSE(none.Accepts(
        R"({"intent":"x","checks":[{"stat":"agility","difficulty":"easy","stakes":"s","on_success":[{"op":"set","path":"a.b","value":"a","reason":"r"}],"on_failure":[]}],"changes":[]})"));
}

TEST_CASE("The resolve grammar bounds every short text and escapes exactly", "[grammar]")
{
    const core::World world = MiniWorld();
    const nlohmann::json& state = world.initialState;
    const test::GbnfMatcher engine(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    const test::GbnfMatcher assisted(turn::BuildResolveGrammar(world, state, core::MemoryMode::Assisted));
    const test::GbnfMatcher lookup(turn::BuildLookupGrammar());
    const int limit = turn::SHORT_TEXT_CHARS;

    // Each context wraps the inner text of one JSON string; the check uses agility and easy, valid in the mini world.
    const std::vector<std::pair<std::string, std::function<std::string(const std::string&)>>> contexts = {
        {"intent", [](const std::string& t) { return R"({"intent":")" + t + R"(","checks":[],"changes":[]})"; }},
        {"stakes",
         [](const std::string& t) {
             return R"({"intent":"x","checks":[{"stat":"agility","difficulty":"easy","stakes":")" + t +
                    R"(","on_success":[],"on_failure":[]}],"changes":[]})";
         }},
        {"reason",
         [](const std::string& t) {
             return ProposalOf(R"({"op":"set","path":"player.location","value":"v","reason":")" + t + R"("})");
         }},
        {"string value",
         [](const std::string& t) {
             return ProposalOf(R"({"op":"set","path":"player.location","value":")" + t + R"(","reason":"r"})");
         }},
        {"npc create string",
         [](const std::string& t) {
             return ProposalOf(R"({"op":"create","path":"npc:1","value":{"type":"npc","fields":{"disposition":"wary"}},"reason":")" +
                               t + R"("})");
         }},
    };

    const std::string multibyte = "\xC3\xA9";
    // Escapes built from a backslash so no tool can decode them into the character.
    const std::string BACKSLASH = "\\";
    const std::string escape = BACKSLASH + "u00e9";
    const std::string upperEscape = BACKSLASH + "u00C9";
    for (const auto& [name, wrap] : contexts) {
        auto accepts = [&](const std::string& inner) { return engine.Accepts(wrap(inner)); };
        INFO(name);
        CHECK(accepts(std::string(static_cast<std::size_t>(limit), 'a')));
        CHECK_FALSE(accepts(std::string(static_cast<std::size_t>(limit) + 1, 'a')));
        CHECK(accepts(Repeat(multibyte, limit)));
        CHECK_FALSE(accepts(Repeat(multibyte, limit + 1)));
        CHECK(accepts(Repeat(escape, limit)));
        CHECK(accepts(Repeat(upperEscape, limit)));
        CHECK_FALSE(accepts(Repeat(upperEscape, limit + 1)));
        CHECK_FALSE(accepts(Repeat(escape, limit + 1)));
        CHECK(accepts(upperEscape + BACKSLASH + "u00aF" + BACKSLASH + "uABCD"));
        for (const std::string& bad : {R"(\x41)", R"(\u12)", R"(\u00G0)", R"(\a)"}) {
            INFO(wrap(bad));
            CHECK_FALSE(accepts(bad));
        }
    }

    // A list item, which needs a state with a list field.
    const nlohmann::json listState = PlayerState(nlohmann::json::object());
    const test::GbnfMatcher lists(
        turn::BuildResolveGrammar(WorldWith(AllFieldsSchema(), listState), listState, core::MemoryMode::Engine));
    auto listItem = [](const std::string& t) {
        return ProposalOf(R"({"op":"add","path":"player.conditions","value":")" + t + R"(","reason":"r"})");
    };
    CHECK(lists.Accepts(listItem(std::string(static_cast<std::size_t>(limit), 'a'))));
    CHECK_FALSE(lists.Accepts(listItem(std::string(static_cast<std::size_t>(limit) + 1, 'a'))));
    CHECK(lists.Accepts(listItem(Repeat(multibyte, limit))));
    CHECK_FALSE(lists.Accepts(listItem(Repeat(multibyte, limit + 1))));
    CHECK(lists.Accepts(listItem(Repeat(escape, limit))));
    CHECK_FALSE(lists.Accepts(listItem(Repeat(escape, limit + 1))));
    CHECK(lists.Accepts(listItem(Repeat(upperEscape, limit))));
    CHECK_FALSE(lists.Accepts(listItem(Repeat(upperEscape, limit + 1))));
    CHECK_FALSE(lists.Accepts(listItem(R"(\u12)")));

    // A lookup query, in the same terms.
    auto query = [](const std::string& t) { return R"({"lookups":[{"source":"lore","query":")" + t + R"("}]})"; };
    CHECK(lookup.Accepts(query(std::string(static_cast<std::size_t>(limit), 'a'))));
    CHECK_FALSE(lookup.Accepts(query(std::string(static_cast<std::size_t>(limit) + 1, 'a'))));
    CHECK(lookup.Accepts(query(Repeat(multibyte, limit))));
    CHECK_FALSE(lookup.Accepts(query(Repeat(multibyte, limit + 1))));
    CHECK(lookup.Accepts(query(Repeat(escape, limit))));
    CHECK_FALSE(lookup.Accepts(query(Repeat(escape, limit + 1))));
    CHECK(lookup.Accepts(query(Repeat(upperEscape, limit))));
    CHECK_FALSE(lookup.Accepts(query(Repeat(upperEscape, limit + 1))));
    for (const std::string& bad : {R"(\x41)", R"(\u12)", R"(\u00G0)"}) {
        INFO(query(bad));
        CHECK_FALSE(lookup.Accepts(query(bad)));
    }

    // Notes, at the long-text bound.
    const int longLimit = turn::LONG_TEXT_CHARS;
    auto note = [](const std::string& t) {
        return R"({"intent":"x","checks":[],"changes":[],"notes":[")" + t + R"("]})";
    };
    CHECK(assisted.Accepts(note(std::string(static_cast<std::size_t>(longLimit), 'a'))));
    CHECK_FALSE(assisted.Accepts(note(std::string(static_cast<std::size_t>(longLimit) + 1, 'a'))));
    CHECK(assisted.Accepts(note(Repeat(escape, longLimit))));
    CHECK_FALSE(assisted.Accepts(note(Repeat(escape, longLimit + 1))));
    CHECK(assisted.Accepts(note(Repeat(upperEscape, longLimit))));
    CHECK_FALSE(assisted.Accepts(note(Repeat(upperEscape, longLimit + 1))));
    CHECK(assisted.Accepts(note(Repeat(multibyte, longLimit))));
    CHECK_FALSE(assisted.Accepts(note(Repeat(multibyte, longLimit + 1))));
    CHECK_FALSE(assisted.Accepts(note(R"(\u12)")));
}

TEST_CASE("The review grammars' variants bound problems and digests exactly", "[grammar]")
{
    const std::string problem200(static_cast<std::size_t>(turn::REVIEW_PROBLEM_MAX_CHARS), 'p');
    const std::string digest300(static_cast<std::size_t>(turn::DIGEST_MAX_CHARS), 'd');
    const std::string BACKSLASH = "\\";
    const std::string escape = BACKSLASH + "u00e9";
    const std::string upperEscape = BACKSLASH + "u00C9";

    const test::GbnfMatcher check(turn::BuildReviewGrammar(true, false));
    auto verdict = [](const std::string& consistent, const std::string& problems) {
        return R"({"consistent":)" + consistent + R"(,"problems":[)" + problems + "]}";
    };
    const std::vector<std::string> checkAccepted = {
        verdict("true", ""), verdict("false", R"("p")"), verdict("false", R"("p","q","r")"),
        verdict("false", "\"" + problem200 + "\""), verdict("false", "\"caf" + escape + " \\\"x\\\"\\n\""),
        verdict("false", "\"" + Repeat(escape, turn::REVIEW_PROBLEM_MAX_CHARS) + "\""),
        verdict("false", "\"" + Repeat(upperEscape, turn::REVIEW_PROBLEM_MAX_CHARS) + "\"")};
    for (const std::string& text : checkAccepted) {
        INFO(text);
        CHECK(check.Accepts(text));
    }
    const std::vector<std::string> checkRejected = {
        verdict("true", R"("p")"), verdict("false", ""), verdict("false", R"("p","q","r","s")"),
        verdict("false", "\"" + problem200 + "p\""), verdict("false", R"("")"), verdict("false", R"("p",)"),
        verdict("false", "\"" + Repeat(escape, turn::REVIEW_PROBLEM_MAX_CHARS + 1) + "\""),
        verdict("false", "\"" + Repeat(upperEscape, turn::REVIEW_PROBLEM_MAX_CHARS + 1) + "\""),
        verdict("false", R"("\u12")"), verdict("maybe", R"("p")")};
    for (const std::string& text : checkRejected) {
        INFO(text);
        CHECK_FALSE(check.Accepts(text));
    }

    const test::GbnfMatcher digest(turn::BuildReviewGrammar(false, true));
    auto digestOf = [](const std::string& text) { return R"({"digest":")" + text + R"("})"; };
    const std::vector<std::string> digestAccepted = {digestOf("d"), digestOf(""), digestOf(digest300),
                                                     digestOf("caf" + escape + " \\\"x\\\"\\n"),
                                                     digestOf(Repeat(escape, turn::DIGEST_MAX_CHARS)),
                                                     digestOf(Repeat(upperEscape, turn::DIGEST_MAX_CHARS))};
    for (const std::string& text : digestAccepted) {
        INFO(text);
        CHECK(digest.Accepts(text));
    }
    const std::vector<std::string> digestRejected = {
        digestOf(digest300 + "d"), digestOf(Repeat(escape, turn::DIGEST_MAX_CHARS + 1)),
        digestOf(Repeat(upperEscape, turn::DIGEST_MAX_CHARS + 1)), digestOf(R"(\u12)"), R"({"consistent":true,"digest":"d"})",
        R"({"digest":"d","consistent":true})", R"({"consistent":true,"problems":[]})", R"({})"};
    for (const std::string& text : digestRejected) {
        INFO(text);
        CHECK_FALSE(digest.Accepts(text));
    }
}

TEST_CASE("The resolve grammar's creates and branches hold their exact shapes", "[grammar]")
{
    const nlohmann::json schema = nlohmann::json::parse(R"({"entity_types": {"pack": {"creatable": true,
        "fields": {"kind": {"type": "enum", "values": ["small", "large"]}, "size": {"type": "int", "min": 0, "max": 9}}}}})");
    const nlohmann::json state = nlohmann::json::object();
    const core::World world = WorldWith(schema, state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    auto create = [](const std::string& id, const std::string& fields) {
        return ProposalOf(R"({"op":"create","path":")" + id + R"(","value":{"type":"pack","fields":)" + fields +
                          R"(},"reason":"r"})");
    };
    CHECK(matcher.Accepts(create("pack:1", R"({"kind":"small","size":1})")));
    const std::vector<std::string> rejected = {
        create("pack:1", R"({"size":1,"kind":"small"})"),
        create("pack:1", R"({"kind":"small","size":1,"extra":1})"),
        create("", R"({"kind":"small","size":1})"),
        create("pack:1", R"({"kind":"small","size":1000000000})"),
        create("pack:1", R"({"kind":"small","size":-1000000000})"),
        create("pack:1", R"({"kind":"small","size":05})")};
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }

    // on_failure holds up to MAX_BRANCH_CHANGES changes, one more is refused.
    const std::string change = R"({"op":"set","path":"pack:1.size","value":1,"reason":"r"})";
    const nlohmann::json heldSchema = nlohmann::json::parse(R"({"entity_types": {"pack": {
        "fields": {"size": {"type": "int", "min": 0, "max": 9}}}}})");
    const nlohmann::json heldState = {{"pack:1", {{"type", "pack"}, {"fields", {{"size", 1}}}}}};
    const core::World heldWorld = WorldWith(heldSchema, heldState);
    const test::GbnfMatcher branches(turn::BuildResolveGrammar(heldWorld, heldState, core::MemoryMode::Engine));
    auto failure = [&](int count) {
        return R"({"intent":"x","checks":[{"stat":"agility","difficulty":"easy","stakes":"s","on_success":[],"on_failure":)" +
               JsonList(change, count) + R"(}],"changes":[]})";
    };
    CHECK(branches.Accepts(failure(turn::MAX_BRANCH_CHANGES)));
    CHECK_FALSE(branches.Accepts(failure(turn::MAX_BRANCH_CHANGES + 1)));
}
