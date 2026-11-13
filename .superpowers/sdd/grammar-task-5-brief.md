### Task 5: Morpheus's grammars on the builder

`BuildResolveGrammar`, `BuildLookupGrammar` and `BuildReviewGrammar` are rewritten with `grammar::GrammarBuilder`; the private helpers go. The language does not change (Task 2's characterization tests, the existing accept/reject tests and the replays stay green). Exact-text tests that see a body with alternatives are updated to the builder's parentheses first, and fail against today's code.

**Files:**
- Modify: `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/turn/CMakeLists.txt`
- Test: `tests/turn/ResolveGrammarTests.cpp`

**Interfaces:**
- Consumes: `grammar::Literal`, `JsonString`, `IntRange`, `Enum`, `ListOf`, `Object`, `ObjectField`, `Alternatives`, `GrammarBuilder` (Tasks 3, 4).
- Produces: the same three functions with the same signatures and languages; `constexpr long long turn::MAX_INT_MAGNITUDE = 999999999;`. `morpheus_turn` links `morpheus_grammar`.

- [ ] **Step 1: Update the exact-text tests to the builder's text**

In `tests/turn/ResolveGrammarTests.cpp` make these replacements.

In "The resolve grammar names the world's stats, difficulties and field shapes", replace

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= "\"easy\"" | "\"hard\"")"));
```

with

```cpp
    // Exact text from the grammar builder: an enum's alternatives come in parentheses.
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= ("\"easy\"" | "\"hard\""))"));
```

and in the same test replace

```cpp
    const std::string items = FindRuleName(grammar, "items", R"("lantern" | item)");
```

with

```cpp
    const std::string items = FindRuleName(grammar, "items", R"(("lantern" | item))"); // The builder's parentheses.
```

Then, in each of the three tests "The resolve grammar names the world's stats, difficulties and field shapes", "The resolve grammar follows the live state and the memory mode" and "The resolve grammar covers list fields and enum sets on existing entities", replace

```cpp
    const std::string disposition = FindRuleName(grammar, "enum", R"("\"wary\"" | "\"friendly\"")");
```

with

```cpp
    const std::string disposition = FindRuleName(grammar, "enum", R"(("\"wary\"" | "\"friendly\""))"); // Builder text.
```

In "The resolve grammar follows the live state and the memory mode", replace

```cpp
    CHECK(CountOccurrences(grammar, R"(::= "\"wary\"")") == 1);
```

with

```cpp
    CHECK(CountOccurrences(grammar, R"(::= ("\"wary\"")") == 1); // The builder's parentheses.
```

In "The resolve grammar bounds every list", replace

```cpp
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= "[]" | "[" check ("," check){0,2} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= "[]" | "[" change ("," change){0,5} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= "[]" | "[" change ("," change){0,3} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= "[]" | "[" long-text ("," long-text){0,1} "]")"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= "[]" | "[" lookup ("," lookup){0,2} "]")"));
```

with

```cpp
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    // Exact text from the grammar builder: a list that may be empty is a group of two alternatives.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= ("[]" | "[" check ("," check){0,2} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= ("[]" | "[" change ("," change){0,5} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= ("[]" | "[" change ("," change){0,3} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= ("[]" | "[" long-text ("," long-text){0,1} "]"))"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= ("[]" | "[" lookup ("," lookup){0,2} "]"))"));
```

In "The resolve grammar escapes enum values", replace

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(::= "\"a\\\"b\\\\c\"" | "\"plain\"")"));
```

with

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(::= ("\"a\\\"b\\\\c\"" | "\"plain\""))")); // The builder's parentheses.
```

In "Held counter items are matched by name next to the generic item rule", replace

```cpp
    const std::string items =
        FindRuleName(grammar, "items", R"("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item)");
```

with

```cpp
    // Exact text from the grammar builder: the alternatives come in parentheses.
    const std::string items =
        FindRuleName(grammar, "items", R"(("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item))");
```

In "The review grammar asks for the verdict, the digest or both", replace

```cpp
    CHECK_THAT(both, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" ",\"digest\":" digest-text "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}")"));
```

with

```cpp
    // Exact text from the grammar builder: two objects as alternatives, each key and value its own fragment.
    CHECK_THAT(both, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" ",\"digest\":" digest-text "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}"))"));
```

and replace

```cpp
    CHECK_THAT(check, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" "}")"));
```

with

```cpp
    CHECK_THAT(check, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" "}"))"));
```

- [ ] **Step 2: Run the tests to verify the updated ones fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: failures in "The resolve grammar names the world's stats, difficulties and field shapes", "The resolve grammar follows the live state and the memory mode", "The resolve grammar bounds every list", "The resolve grammar covers list fields and enum sets on existing entities", "The resolve grammar escapes enum values", "Held counter items are matched by name next to the generic item rule" and "The review grammar asks for the verdict, the digest or both" (today's text has no parentheses); the six characterization tests still pass.

- [ ] **Step 3: Link the library into `turn`**

In `src/turn/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context)
```

with

```cmake
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_grammar)
```

- [ ] **Step 4: Rewrite the header**

Replace the whole of `src/turn/ResolveGrammar.h` with:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"
#include "core/World.h"

namespace morpheus::turn {

// Size limits the grammars enforce. They bound the resolve output, and so the resolution record, and keep the
// model from rambling inside JSON strings.
constexpr int MAX_CHECKS         = 3;   // Checks per turn.
constexpr int MAX_CHANGES        = 6;   // Unconditional changes.
constexpr int MAX_BRANCH_CHANGES = 4;   // Per check outcome.
constexpr int MAX_NOTES          = 2;   // Assisted mode.
constexpr int MAX_LOOKUPS        = 3;   // Assisted mode.
constexpr int MAX_LIST_ITEMS     = 8;   // List and counter values inside a create.
constexpr int SHORT_TEXT_CHARS   = 120; // intent, stakes, reasons, string values, lookup queries.
constexpr int LONG_TEXT_CHARS    = 400; // Notes.
constexpr int MAX_NEW_ID_CHARS   = 40;
constexpr int MAX_ITEM_CHARS     = 32;  // New counter items; held items are matched by name.
constexpr int REVIEW_MAX_PROBLEMS      = 3;   // Contradictions one review may list.
constexpr int REVIEW_PROBLEM_MAX_CHARS = 200; // Each of them.
constexpr int DIGEST_MAX_CHARS         = 300; // A turn's digest.
constexpr long long MAX_INT_MAGNITUDE  = 999999999; // Integers have at most 9 digits, so they always fit an int.

// The three grammars are built with grammar::GrammarBuilder, so each starts with its root line.

// GBNF for the resolve call (architecture spec, "Turn flow"). The output is one compact JSON object with its keys
// in this order:
//   {"intent": str, "checks": [check], "changes": [change]}, plus "notes": [str] in assisted mode
//   check:  {"stat", "difficulty", "stakes": str, "on_success": [change], "on_failure": [change]}
//   change: {"op", "path", "value" (absent for a counter item remove), "reason": str}
// Only shapes the schema allows can be written: for every field of every entity in `state`, int set/add, enum
// set (its values), string set, list add/remove, counter item add (non-zero) and remove; and create of a
// creatable type with a new id and every field of that type in name order. Integers are exact: from
// -MAX_INT_MAGNITUDE to MAX_INT_MAGNITUDE, without leading zeros or "-0". Counter item names: a new item is
// written as [a-z0-9_]{1,MAX_ITEM_CHARS}; an item the entity holds now is also matched by its own name, verbatim,
// so names such as "Rusty Knife" can be added to and removed. Stats and difficulties are the world's. Applying
// still validates values (ranges, existing ids). `state` must be validated state.
// When no entity has a field and no type is creatable, no change can be written: the grammar's `changes` and
// branch lists can then only be "[]".
std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode);

// GBNF for the assisted-mode lookup call: {"lookups": [{"source": "lore"|"history"|"notes", "query": str}]}, at
// most MAX_LOOKUPS entries. Its first line, the root, is how turn::CallKindOf tells a lookup call.
std::string BuildLookupGrammar();

// GBNF for the review call (architecture spec, "Turn flow"). Its first line is always "root ::= review". The output
// is one compact JSON object:
//   check:  {"consistent":true,"problems":[]} or {"consistent":false,"problems":[1 to REVIEW_MAX_PROBLEMS strings of
//           at most REVIEW_PROBLEM_MAX_CHARS]}
//   digest: "digest": a string of at most DIGEST_MAX_CHARS, after the verdict, or alone: {"digest": str}
// Throws ContractError unless `check` or `digest` is set.
std::string BuildReviewGrammar(bool check, bool digest);

} // namespace morpheus::turn
```

- [ ] **Step 5: Rewrite the builders**

Replace the whole of `src/turn/ResolveGrammar.cpp` with:

```cpp
#include "turn/ResolveGrammar.h"

#include <format>
#include <map>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "grammar/Gbnf.h"

namespace morpheus::turn {

std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode)
{
    MORPHEUS_REQUIRE(state.is_object(), "BuildResolveGrammar: state must be validated state");
    const bool assisted = mode == core::MemoryMode::Assisted;
    grammar::GrammarBuilder builder;

    std::vector<std::string> changes;          // Names of the change-N rules.
    std::map<std::string, std::string> enums;  // "type.field" -> enum-N rule name.
    int itemRuleCount = 0;                     // items-N rules written so far.

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
                const std::string name = builder.Rule(std::format("enum-{}", enums.size()), grammar::Enum(def.values));
                it = enums.emplace(typeName + "." + fieldName, name).first;
            }
            return it->second;
        }
        }
        throw core::ContractError("BuildResolveGrammar: unhandled FieldType");
    };
    // Each change shape is a rule of its own, change-N, and `change` is any one of them.
    auto addChange = [&](std::string body) {
        changes.push_back(builder.Rule(std::format("change-{}", changes.size()), std::move(body)));
    };

    // Changes to the fields of every entity that exists now. Each body is a sequence of literals around the value
    // rule; the `reason` rule closes the change object.
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const std::string typeName = entity.value().at("type").get<std::string>();
        const core::EntityTypeDef* pType = world.schema.FindEntityType(typeName);
        MORPHEUS_REQUIRE(pType != nullptr, "BuildResolveGrammar: state has an unknown entity type");
        for (const auto& [fieldName, def] : pType->fields) {
            const std::string path = entity.key() + "." + fieldName;
            const std::string pathValue = grammar::Literal(",\"path\":\"" + path + "\",\"value\":");
            switch (def.type) {
            case core::FieldType::Int:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"set", "add"}) + " " + pathValue +
                          " int reason");
                break;
            case core::FieldType::Enum:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " " +
                          valueRule(typeName, fieldName, def) + " reason");
                break;
            case core::FieldType::String:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " short-text reason");
                break;
            case core::FieldType::List:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"add", "remove"}) + " " + pathValue +
                          " short-text reason");
                break;
            case core::FieldType::Counter: {
                // The item part of the path: an item held now, written by name (so "Rusty Knife" or "water-flask"
                // can be changed), or the generic rule for a new item. Nothing held means just the generic rule.
                std::string itemRule = "item";
                const nlohmann::json& held = entity.value().at("fields").at(fieldName);
                if (!held.empty()) {
                    std::vector<std::string> names;
                    for (const auto& [itemName, count] : held.items()) {
                        // The name sits inside the JSON path string, so match its JSON-escaped form without the
                        // surrounding quotes, then escape that once more as a GBNF literal.
                        const std::string escaped = nlohmann::json(itemName).dump();
                        names.push_back(grammar::Literal(escaped.substr(1, escaped.size() - 2)));
                    }
                    names.push_back("item");
                    itemRule = builder.Rule(std::format("items-{}", itemRuleCount++), grammar::Alternatives(names));
                }
                // Add a non-zero count to the item, or remove the item (no value).
                addChange(grammar::Alternatives(
                              {grammar::Literal("{\"op\":\"add\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\",\"value\":") + " nonzero-int",
                               grammar::Literal("{\"op\":\"remove\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\"")}) +
                          " reason");
                break;
            }
            }
        }
    }

    // New entities of creatable types, every field given, in name order.
    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        if (!type.creatable) {
            continue;
        }
        std::string fields;
        for (const auto& [fieldName, def] : type.fields) {
            fields += " " + grammar::Literal((fields.empty() ? "\"" : ",\"") + fieldName + "\":") + " " +
                      valueRule(typeName, fieldName, def);
        }
        addChange(grammar::Literal("{\"op\":\"create\",\"path\":") + " new-id " +
                  grammar::Literal(",\"value\":{\"type\":\"" + typeName + "\",\"fields\":{") + fields + " " +
                  grammar::Literal("}}") + " reason");
    }

    std::vector<std::string> difficulties;
    for (const auto& [name, target] : world.difficulties) {
        difficulties.push_back(name);
    }

    // The proposal's structure: checks with their two outcome branches, the unconditional changes and, in assisted
    // mode, notes. With nothing the model could change (no entity fields, no creatable type) the change lists can
    // only be empty and there is no `change` rule to reference.
    builder.Rule("checks", grammar::ListOf("check", 0, MAX_CHECKS));
    builder.Rule("check", grammar::Object({{"stat", "stat"},
                                           {"difficulty", "difficulty"},
                                           {"stakes", "short-text"},
                                           {"on_success", "branch"},
                                           {"on_failure", "branch"}}));
    builder.Rule("branch", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_BRANCH_CHANGES));
    builder.Rule("changes", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_CHANGES));
    if (assisted) {
        builder.Rule("notes", grammar::ListOf("long-text", 0, MAX_NOTES));
        builder.Rule("long-text", grammar::JsonString(0, LONG_TEXT_CHARS));
    }
    builder.Rule("stat", grammar::Enum(world.stats));
    builder.Rule("difficulty", grammar::Enum(difficulties));
    if (!changes.empty()) {
        builder.Rule("change", grammar::Alternatives(changes));
    }

    // The value rules. Integers are exact ranges of at most 9 digits; a counter add is never zero.
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    builder.Rule("int", grammar::IntRange(-MAX_INT_MAGNITUDE, MAX_INT_MAGNITUDE));
    builder.Rule("nonzero-int", grammar::Alternatives({grammar::IntRange(-MAX_INT_MAGNITUDE, -1),
                                                       grammar::IntRange(1, MAX_INT_MAGNITUDE)}));
    builder.Rule("pos-int", grammar::IntRange(1, MAX_INT_MAGNITUDE));
    builder.Rule("item", std::format("[a-z0-9_]{{1,{}}}", MAX_ITEM_CHARS));
    builder.Rule("new-id", grammar::Literal("\"") + std::format(" [a-z0-9_:]{{1,{}}} ", MAX_NEW_ID_CHARS) +
                               grammar::Literal("\""));
    builder.Rule("text-list", grammar::ListOf("short-text", 0, MAX_LIST_ITEMS));
    // A counter value: {} or up to MAX_LIST_ITEMS new items, each with a positive count.
    builder.Rule("counter-value",
                 grammar::Alternatives({grammar::Literal("{}"),
                                        grammar::Literal("{\"") + " item " + grammar::Literal("\":") + " pos-int (" +
                                            grammar::Literal(",\"") + " item " + grammar::Literal("\":") +
                                            std::format(" pos-int){{0,{}}} ", MAX_LIST_ITEMS - 1) + grammar::Literal("}")}));
    // Every change ends with its reason, which closes the change object.
    builder.Rule("reason", grammar::Literal(",\"reason\":") + " short-text " + grammar::Literal("}"));

    std::vector<grammar::ObjectField> root = {{"intent", "short-text"}, {"checks", "checks"}, {"changes", "changes"}};
    if (assisted) {
        root.push_back({"notes", "notes"});
    }
    return builder.Text(grammar::Object(root));
}

std::string BuildLookupGrammar()
{
    grammar::GrammarBuilder builder;
    builder.Rule("lookups", grammar::ListOf("lookup", 0, MAX_LOOKUPS));
    builder.Rule("lookup", grammar::Object({{"source", grammar::Enum({"lore", "history", "notes"})},
                                            {"query", "short-text"}}));
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    // The root line, {"lookups": [...]}, is how CallKindOf recognises a lookup call.
    return builder.Text(grammar::Object({{"lookups", "lookups"}}));
}

std::string BuildReviewGrammar(bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "BuildReviewGrammar: needs the check, the digest or both");
    grammar::GrammarBuilder builder;
    std::string review;
    if (check) {
        // Consistent with no problems, or inconsistent with 1 to REVIEW_MAX_PROBLEMS of them; the digest, when it is
        // asked for, follows the verdict.
        std::vector<grammar::ObjectField> consistent = {{"consistent", grammar::Literal("true")},
                                                        {"problems", grammar::Literal("[]")}};
        std::vector<grammar::ObjectField> inconsistent = {
            {"consistent", grammar::Literal("false")},
            {"problems", grammar::ListOf("problem-text", 1, REVIEW_MAX_PROBLEMS)}};
        if (digest) {
            consistent.push_back({"digest", "digest-text"});
            inconsistent.push_back({"digest", "digest-text"});
        }
        review = grammar::Alternatives({grammar::Object(consistent), grammar::Object(inconsistent)});
    } else {
        // The digest alone.
        review = grammar::Object({{"digest", "digest-text"}});
    }
    builder.Rule("review", review);
    if (check) {
        builder.Rule("problem-text", grammar::JsonString(1, REVIEW_PROBLEM_MAX_CHARS));
    }
    if (digest) {
        builder.Rule("digest-text", grammar::JsonString(0, DIGEST_MAX_CHARS));
    }
    // A fixed root line, whatever is asked for, so the call is recognised by it (CallKindOf).
    return builder.Text("review");
}

} // namespace morpheus::turn
```

- [ ] **Step 6: Run the grammar tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: builds with no warnings; `All tests passed` (18 test cases: the twelve existing, with their updated text, and the six characterization tests).

- [ ] **Step 7: Run the call-kind, replay, session and review tests**

Run: `./build/tests/bin/Debug/morpheus_tests.exe "[replay],[session],[review],[model_seat]"`
Expected: `All tests passed`. `CallKindOf` still recognises the lookup and review grammars by their first lines (and the model seat labels calls through it), every replay fixture still replays, and the session tests that compare `spec.grammar` with `BuildReviewGrammar(...)` still match.

- [ ] **Step 8: Confirm the old helpers are gone**

Run: `grep -n "Terminals\|TERMINALS\|namespace {" src/turn/ResolveGrammar.cpp`
Expected: no output (no private helpers, no copies).

- [ ] **Step 9: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 335`.

- [ ] **Step 10: Commit**

```bash
git add src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/turn/CMakeLists.txt tests/turn/ResolveGrammarTests.cpp
git commit -m "Build the resolve, lookup and review grammars on the grammar builder" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

