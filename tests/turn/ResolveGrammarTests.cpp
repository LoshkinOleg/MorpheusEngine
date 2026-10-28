#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
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
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= "\"easy\"" | "\"hard\"")"));
    // An int field: set or add an integer.
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":" ("\"set\"" | "\"add\"") ",\"path\":\"player.health\",\"value\":" int reason)"));
    // A string field: set only.
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"set\"" ",\"path\":\"player.location\",\"value\":" short-text reason)"));
    // A counter field: add a non-zero count to an item, or remove the item. The player holds a lantern, so the item
    // part is a per-field rule that matches it by name or any new item.
    const std::string items = FindRuleName(grammar, "items", R"("lantern" | item)");
    REQUIRE_FALSE(items.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"add\",\"path\":\"player.inventory." )" + items + R"( "\",\"value\":" nonzero-int)"));
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"remove\",\"path\":\"player.inventory." )" + items + R"( "\"")"));
    // Create: a new id and every field of the creatable type.
    const std::string disposition = FindRuleName(grammar, "enum", R"("\"wary\"" | "\"friendly\"")");
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
    const std::string disposition = FindRuleName(grammar, "enum", R"("\"wary\"" | "\"friendly\"")");
    REQUIRE_FALSE(disposition.empty());
    CHECK_THAT(grammar, ContainsSubstring(
        R"("{\"op\":\"set\"" ",\"path\":\"npc:marta.disposition\",\"value\":" )" + disposition + " reason"));
    CHECK_THAT(grammar, ContainsSubstring(R"("\"disposition\":" )" + disposition + R"( "}}" reason)"));
    CHECK(CountOccurrences(grammar, R"(::= "\"wary\"")") == 1);
}

TEST_CASE("The resolve grammar bounds every list", "[grammar]")
{
    const core::World world = MiniWorld();
    const std::string grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted);
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= "[]" | "[" check ("," check){0,2} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= "[]" | "[" change ("," change){0,5} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= "[]" | "[" change ("," change){0,3} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= "[]" | "[" long-text ("," long-text){0,1} "]")"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= "[]" | "[" lookup ("," lookup){0,2} "]")"));
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
    const std::string disposition = FindRuleName(grammar, "enum", R"("\"wary\"" | "\"friendly\"")");
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

    CHECK_THAT(grammar, ContainsSubstring(R"(::= "\"a\\\"b\\\\c\"" | "\"plain\"")"));
    CHECK(GrammarProblem(grammar).empty());
}

TEST_CASE("Held counter items are matched by name next to the generic item rule", "[grammar]")
{
    // "Rusty Knife" and "water-flask" cannot be spelled by [a-z0-9_]; the quoted name needs escaping twice.
    const nlohmann::json state = PlayerState({{"Rusty Knife", 1}, {"water-flask", 2}, {"say \"hi\"", 1}});
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const std::string grammar = turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine);

    // The rule lists the held names (a std::map order: capital letters first) and still allows a new item.
    const std::string items =
        FindRuleName(grammar, "items", R"("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item)");
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
