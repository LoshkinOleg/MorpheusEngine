### Task 2: Characterize what Morpheus's grammars accept

Before the rewrite, pin the language of today's resolve, lookup and review grammars with accept/reject tests on the matcher. They pass on today's code (that is their point) and must pass unchanged after Task 5. If one fails here, the matcher or the expectation is wrong: fix the matcher or the test, never `ResolveGrammar.cpp`, and say so in the task report.

**Files:**
- Modify: `tests/turn/ResolveGrammarTests.cpp`

**Interfaces:**
- Consumes: `test::GbnfMatcher` (Task 1); `turn::BuildResolveGrammar`, `turn::BuildLookupGrammar`, `turn::BuildReviewGrammar` and the size constants in `turn/ResolveGrammar.h` (unchanged).
- Produces: six `[grammar]` test cases that Task 5 must keep green.

- [ ] **Step 1: Add the include and a list helper**

In `tests/turn/ResolveGrammarTests.cpp`, replace

```cpp
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
```

with

```cpp
#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <vector>
```

replace

```cpp
#include "TestPaths.h"
```

with

```cpp
#include "TestPaths.h"
#include "grammar/GbnfMatcher.h"
```

and replace

```cpp
std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}
```

with

```cpp
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
```

- [ ] **Step 2: Write the characterization tests**

Append to `tests/turn/ResolveGrammarTests.cpp`:

```cpp
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
```

- [ ] **Step 3: Run the tests: they pass on today's grammars**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: builds with no warnings; `All tests passed` (18 test cases: the twelve existing `[grammar]` cases and these six). A failure here is a bug in the matcher or in the expectation (see the task intro), not in the grammar.

- [ ] **Step 4: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 324`.

- [ ] **Step 5: Commit**

```bash
git add tests/turn/ResolveGrammarTests.cpp
git commit -m "Characterize what the resolve, lookup and review grammars accept" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

