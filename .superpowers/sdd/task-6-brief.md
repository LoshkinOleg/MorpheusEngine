### Task 6: The turn module and the resolve grammar

**Files:**
- Create: `src/turn/CMakeLists.txt`, `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `tests/turn/ResolveGrammarTests.cpp`, `tests/llama/LlamaGrammarTests.cpp`
- Modify: `CMakeLists.txt` (root), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::World`, `core::StateSchema::EntityTypes()`, `core::MemoryMode`.
- Produces:
  - The constants `MAX_CHECKS`, `MAX_CHANGES`, `MAX_BRANCH_CHANGES`, `MAX_NOTES`, `MAX_LOOKUPS`, `MAX_LIST_ITEMS`, `SHORT_TEXT_CHARS`, `LONG_TEXT_CHARS`, `MAX_NEW_ID_CHARS` and `MAX_ITEM_CHARS`
  - `std::string turn::BuildResolveGrammar(const core::World&, const nlohmann::json& state, core::MemoryMode)`
  - `std::string turn::BuildLookupGrammar()`
  - The target `morpheus_turn`

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/ResolveGrammarTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/World.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

} // namespace

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
    // A counter field: add a non-zero count to an item, or remove the item.
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"add\",\"path\":\"player.inventory." item "\",\"value\":" nonzero-int)"));
    CHECK_THAT(grammar, ContainsSubstring(R"("{\"op\":\"remove\",\"path\":\"player.inventory." item "\"")"));
    // Create: a new id and every field of the creatable type.
    CHECK_THAT(grammar, ContainsSubstring(
        R"(",\"value\":{\"type\":\"npc\",\"fields\":{" "\"disposition\":" enum-0 "}}" reason)"));
    CHECK_THAT(grammar, ContainsSubstring(R"(enum-0 ::= "\"wary\"" | "\"friendly\"")"));
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
}

TEST_CASE("The lookup grammar lists sources and queries", "[grammar]")
{
    const std::string grammar = turn::BuildLookupGrammar();
    CHECK_THAT(grammar, ContainsSubstring(R"(root ::= "{\"lookups\":" lookups "}")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(("\"lore\"" | "\"history\"" | "\"notes\""))"));
}
```

Create `tests/llama/LlamaGrammarTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"

using namespace morpheus;

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
    spec.maxTokens = 512;
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
}
```

Add `#include <algorithm>` for `std::max`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: the build fails. `turn/ResolveGrammar.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/CMakeLists.txt`:

```cmake
add_library(morpheus_turn STATIC
    ResolveGrammar.cpp
)
target_include_directories(morpheus_turn PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context)
morpheus_target_defaults(morpheus_turn)
```

Root `CMakeLists.txt`: add `add_subdirectory(src/turn)` after `add_subdirectory(src/context)`. In `tests/CMakeLists.txt`, add `morpheus_turn` to `target_link_libraries(morpheus_tests PRIVATE ...)`.

Create `src/turn/ResolveGrammar.h`:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"
#include "core/World.h"

namespace morpheus::turn {

// Size limits the grammars enforce. They bound the resolve output, and so the resolution record, and keep the
// model from rambling inside JSON strings.
constexpr int MAX_CHECKS = 3;
constexpr int MAX_CHANGES = 6;        // Unconditional changes.
constexpr int MAX_BRANCH_CHANGES = 4; // Per check outcome.
constexpr int MAX_NOTES = 2;          // Assisted mode.
constexpr int MAX_LOOKUPS = 3;        // Assisted mode.
constexpr int MAX_LIST_ITEMS = 8;     // List and counter values inside a create.
constexpr int SHORT_TEXT_CHARS = 120; // intent, stakes, reasons, string values, lookup queries.
constexpr int LONG_TEXT_CHARS = 400;  // Notes.
constexpr int MAX_NEW_ID_CHARS = 40;
constexpr int MAX_ITEM_CHARS = 32;

// GBNF for the resolve call (architecture spec, "Turn flow"). The output is one compact JSON object with its keys
// in this order:
//   {"intent": str, "checks": [check], "changes": [change]}, plus "notes": [str] in assisted mode
//   check:  {"stat", "difficulty", "stakes": str, "on_success": [change], "on_failure": [change]}
//   change: {"op", "path", "value" (absent for a counter item remove), "reason": str}
// Only shapes the schema allows can be written: for every field of every entity in `state`, int set/add, enum
// set (its values), string set, list add/remove, counter item add (non-zero) and remove; and create of a
// creatable type with a new id and every field of that type in name order. Stats and difficulties are the
// world's. Applying still validates values (ranges, existing ids). `state` must be validated state.
std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode);

// GBNF for the assisted-mode lookup call: {"lookups": [{"source": "lore"|"history"|"notes", "query": str}]}, at
// most MAX_LOOKUPS entries.
std::string BuildLookupGrammar();

} // namespace morpheus::turn
```

Create `src/turn/ResolveGrammar.cpp`:

```cpp
#include "turn/ResolveGrammar.h"

#include <format>
#include <map>
#include <string_view>
#include <vector>

#include "core/Errors.h"

namespace morpheus::turn {

namespace {

// Shared terminals. Strings are bounded JSON strings; integers have at most 9 digits, so they always fit an int.
// Placeholders: {0} short text, {1} long text, {2} item name, {3} new id length, {4} extra list items.
constexpr std::string_view TERMINALS = R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{{4}})
short-text ::= "\"" char{{0,{0}}} "\""
long-text ::= "\"" char{{0,{1}}} "\""
int ::= "0" | "-"? [1-9] [0-9]{{0,8}}
nonzero-int ::= "-"? [1-9] [0-9]{{0,8}}
pos-int ::= [1-9] [0-9]{{0,8}}
item ::= [a-z0-9_]{{1,{2}}}
new-id ::= "\"" [a-z0-9_:]{{1,{3}}} "\""
text-list ::= "[]" | "[" short-text ("," short-text){{0,{4}}} "]"
counter-value ::= "{{}}" | "{{\"" item "\":" pos-int (",\"" item "\":" pos-int){{0,{4}}} "}}"
reason ::= ",\"reason\":" short-text "}}"
)gbnf";

std::string Terminals()
{
    return std::format(TERMINALS, SHORT_TEXT_CHARS, LONG_TEXT_CHARS, MAX_ITEM_CHARS, MAX_NEW_ID_CHARS,
                       MAX_LIST_ITEMS - 1);
}

// A GBNF string literal that matches `text` exactly.
std::string Literal(std::string_view text)
{
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result + "\"";
}

// Alternatives matching each value as a JSON string.
std::string Alternatives(const std::vector<std::string>& values)
{
    MORPHEUS_REQUIRE(!values.empty(), "Alternatives: needs at least one value");
    std::string result;
    for (const std::string& value : values) {
        result += (result.empty() ? "" : " | ") + Literal(nlohmann::json(value).dump());
    }
    return result;
}

// A JSON array of 0 to maxItems `element`s.
std::string ListOf(std::string_view element, int maxItems)
{
    MORPHEUS_REQUIRE(maxItems >= 2, "ListOf: maxItems must be at least 2");
    return std::format(R"("[]" | "[" {0} ("," {0}){{0,{1}}} "]")", element, maxItems - 1);
}

} // namespace

std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode)
{
    MORPHEUS_REQUIRE(state.is_object(), "BuildResolveGrammar: state must be validated state");
    const bool assisted = mode == core::MemoryMode::Assisted;

    std::string rules;                           // change-N and enum-N definitions.
    std::vector<std::string> changes;            // Names of the change-N rules.
    std::map<std::string, std::string> enums;    // "type.field" -> enum-N rule name.

    // The value rule for a field; an enum gets its own rule the first time it is used.
    auto valueRule = [&](const std::string& typeName, const std::string& fieldName, const core::FieldDef& def) {
        switch (def.type) {
        case core::FieldType::Int:
            return std::string("int");
        case core::FieldType::String:
            return std::string("short-text");
        case core::FieldType::List:
            return std::string("text-list");
        case core::FieldType::Counter:
            return std::string("counter-value");
        case core::FieldType::Enum: {
            auto it = enums.find(typeName + "." + fieldName);
            if (it == enums.end()) {
                const std::string name = std::format("enum-{}", enums.size());
                rules += name + " ::= " + Alternatives(def.values) + "\n";
                it = enums.emplace(typeName + "." + fieldName, name).first;
            }
            return it->second;
        }
        }
        throw core::ContractError("BuildResolveGrammar: unhandled FieldType");
    };
    auto addChange = [&](const std::string& body) {
        changes.push_back(std::format("change-{}", changes.size()));
        rules += changes.back() + " ::= " + body + "\n";
    };

    // Changes to the fields of every entity that exists now.
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const core::EntityTypeDef* pType = world.schema.FindEntityType(entity.value().at("type").get<std::string>());
        MORPHEUS_REQUIRE(pType != nullptr, "BuildResolveGrammar: state has an unknown entity type");
        for (const auto& [fieldName, def] : pType->fields) {
            const std::string path = entity.key() + "." + fieldName;
            const std::string pathValue = Literal(",\"path\":\"" + path + "\",\"value\":");
            switch (def.type) {
            case core::FieldType::Int:
                addChange(Literal("{\"op\":") + " (" + Literal("\"set\"") + " | " + Literal("\"add\"") + ") " +
                          pathValue + " int reason");
                break;
            case core::FieldType::Enum:
                addChange(Literal("{\"op\":\"set\"") + " " + pathValue + " " +
                          valueRule(entity.value().at("type").get<std::string>(), fieldName, def) + " reason");
                break;
            case core::FieldType::String:
                addChange(Literal("{\"op\":\"set\"") + " " + pathValue + " short-text reason");
                break;
            case core::FieldType::List:
                addChange(Literal("{\"op\":") + " (" + Literal("\"add\"") + " | " + Literal("\"remove\"") + ") " +
                          pathValue + " short-text reason");
                break;
            case core::FieldType::Counter:
                addChange("(" + Literal("{\"op\":\"add\",\"path\":\"" + path + ".") + " item " +
                          Literal("\",\"value\":") + " nonzero-int | " +
                          Literal("{\"op\":\"remove\",\"path\":\"" + path + ".") + " item " + Literal("\"") +
                          ") reason");
                break;
            }
        }
    }

    // New entities of creatable types, every field given.
    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        if (!type.creatable) {
            continue;
        }
        std::string fields;
        for (const auto& [fieldName, def] : type.fields) {
            fields += " " + Literal((fields.empty() ? "\"" : ",\"") + fieldName + "\":") + " " +
                      valueRule(typeName, fieldName, def);
        }
        addChange(Literal("{\"op\":\"create\",\"path\":") + " new-id " +
                  Literal(",\"value\":{\"type\":\"" + typeName + "\",\"fields\":{") + fields + " " + Literal("}}") +
                  " reason");
    }
    MORPHEUS_REQUIRE(!changes.empty(), "BuildResolveGrammar: the state has no fields to change");

    std::vector<std::string> difficulties;
    for (const auto& [name, target] : world.difficulties) {
        difficulties.push_back(name);
    }

    std::string grammar = "root ::= " + Literal("{\"intent\":") + " short-text " + Literal(",\"checks\":") +
                          " checks " + Literal(",\"changes\":") + " changes " +
                          (assisted ? Literal(",\"notes\":") + " notes " : "") + Literal("}") + "\n";
    grammar += "checks ::= " + ListOf("check", MAX_CHECKS) + "\n";
    grammar += "check ::= " + Literal("{\"stat\":") + " stat " + Literal(",\"difficulty\":") + " difficulty " +
               Literal(",\"stakes\":") + " short-text " + Literal(",\"on_success\":") + " branch " +
               Literal(",\"on_failure\":") + " branch " + Literal("}") + "\n";
    grammar += "branch ::= " + ListOf("change", MAX_BRANCH_CHANGES) + "\n";
    grammar += "changes ::= " + ListOf("change", MAX_CHANGES) + "\n";
    if (assisted) {
        grammar += "notes ::= " + ListOf("long-text", MAX_NOTES) + "\n";
    }
    grammar += "stat ::= " + Alternatives(world.stats) + "\n";
    grammar += "difficulty ::= " + Alternatives(difficulties) + "\n";
    std::string changeAlternatives;
    for (const std::string& name : changes) {
        changeAlternatives += (changeAlternatives.empty() ? "" : " | ") + name;
    }
    grammar += "change ::= " + changeAlternatives + "\n";
    return grammar + rules + Terminals();
}

std::string BuildLookupGrammar()
{
    std::string grammar = "root ::= " + Literal("{\"lookups\":") + " lookups " + Literal("}") + "\n";
    grammar += "lookups ::= " + ListOf("lookup", MAX_LOOKUPS) + "\n";
    grammar += "lookup ::= " + Literal("{\"source\":") + " (" + Literal("\"lore\"") + " | " +
               Literal("\"history\"") + " | " + Literal("\"notes\"") + ") " + Literal(",\"query\":") +
               " short-text " + Literal("}") + "\n";
    return grammar + Terminals();
}

} // namespace morpheus::turn
```

Notes on the implementation:
- `TERMINALS` is a `std::format` string, so the GBNF braces are doubled (`{{4}}` renders as `{4}`).
- The literal pieces produce the exact strings the unit tests look for. If a test string and the generator disagree on spacing, fix the generator so the GBNF stays valid, then align the test.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: PASS.

Run: `cmake --preset dev && cmake --build --preset dev`, then, with the model variables exported:
`./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`
Expected: PASS. The `WARN` line shows the resolve output and the tok/s figure. Put both in the report.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/turn tests/CMakeLists.txt tests/turn tests/llama/LlamaGrammarTests.cpp
git commit -m "Add the turn module with schema-generated resolve and lookup grammars" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

