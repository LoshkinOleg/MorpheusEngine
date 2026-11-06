### Task 6: Inspector helpers

These are the pure functions the inspectors draw from:
- how a state node is marked for the latest turn's changes;
- a prompt split into titled sections, or as one raw text;
- one call's numbers from the turn telemetry;
- the latest turn's retrieval hits with their text;
- a check written out in full.

**Files:**
- Create: `src/app/model/Inspect.h`, `src/app/model/Inspect.cpp`, `tests/app/InspectTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `model/Inspect.cpp`)

**Interfaces:**
- Consumes:
  - `core::AppliedChange`, `core::CheckOutcome`, `core::TurnRecord`, `core::Note`;
  - `inference::Prompt`, `inference::RoleName`;
  - the telemetry shape written by `Session::PlayTurn`. `timings[call]` has `prompt_tokens`, `reused`, `evaluated`, `generated`, `prompt_ms`, `generate_ms` and `stopped_at_limit`, where `call` is `"lookup"`, `"resolve"` or `"narrate"`. `timings["retrieval"]` is an array of `{source: "lore"|"history"|"notes", ref, score}`.
- Produces (namespace `morpheus::app`):
  - `enum class ChangeMark { None, Changed, Contains }`.
  - `struct PromptSection { std::string title; std::string text; }`.
  - `struct CallStats { int promptTokens, reused, evaluated, generated; double promptMs, generateMs; bool stoppedAtLimit; }`.
  - `struct RetrievalRow { std::string source; std::string ref; double score; std::string text; }`.
  - `ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied)`.
  - `std::vector<PromptSection> PromptSections(const inference::Prompt& prompt)`.
  - `std::string RawPrompt(const inference::Prompt& prompt)`.
  - `std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call)`.
  - `std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history, const std::vector<core::Note>& notes)`.
  - `std::string CheckText(const core::CheckOutcome& check)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/app/InspectTests.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `model/Inspect.cpp` to `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/model/Inspect.h` is missing.

- [ ] **Step 3: Write the header**

Create `src/app/model/Inspect.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::app {

// How the State window marks a node for the latest turn's applied changes.
enum class ChangeMark {
    None,
    Changed,  // A change targets this node, or a node it lies inside (a created entity).
    Contains, // A change targets something inside this node.
};

// One message of a prompt, for the Context window's formatted view.
struct PromptSection {
    std::string title = {}; // e.g. "#3 user - L3 transcript"
    std::string text = {};
};

// One model call's numbers from the turn telemetry (TurnRecord::timings).
struct CallStats {
    int promptTokens = 0;
    int reused = 0;    // Prompt tokens served from the KV cache.
    int evaluated = 0; // Prompt tokens computed.
    int generated = 0;
    double promptMs = 0.0;
    double generateMs = 0.0;
    bool stoppedAtLimit = false; // Generation hit its max_tokens.
};

// One retrieval hit that reached the turn's resolve prompt.
struct RetrievalRow {
    std::string source = {}; // "lore", "history" or "notes"
    std::string ref = {};
    double score = 0.0;
    std::string text = {}; // The lore subject, the turn's input, or the note.
};

// The mark of the state node at `path`: an entity ("player"), a field ("player.location") or a counter item
// ("player.inventory.lantern"). Paths nest on '.' boundaries, as change paths do.
ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied);

// The prompt's messages, each titled with its index, role and layer: message 0 is the system message (L0-L2),
// messages before stableCount are the L3 transcript, and the rest is the tail (T1-T4).
std::vector<PromptSection> PromptSections(const inference::Prompt& prompt);

// The whole prompt as one copyable text: each message under a "=== role ===" line.
std::string RawPrompt(const inference::Prompt& prompt);

// The numbers of call `call` ("lookup", "resolve" or "narrate") in a turn's telemetry; nullopt when the turn made
// no such call.
std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call);

// The retrieval hits in a turn's telemetry, with their text: a lore hit shows its subject (its ref), a history hit
// the input of that turn, a note hit the note ("<turn>.<index>": that turn's index-th note). Throws DataError when a
// hit names a turn or note the run does not hold.
std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history,
                                        const std::vector<core::Note>& notes);

// A check written out: "agility vs hard: rolled 14 + 3 = 17 against 16, success". The stat part is left out when
// the dice rule adds no stat (statValue 0).
std::string CheckText(const core::CheckOutcome& check);

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/Inspect.cpp`:

```cpp
#include "app/model/Inspect.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <system_error>

#include "core/Errors.h"

namespace morpheus::app {

ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied)
{
    // Whether `inner` is `outer` or lies inside it, nesting on '.' boundaries ("npc_10" is not inside "npc_1").
    auto within = [](std::string_view inner, std::string_view outer) {
        return inner.substr(0, outer.size()) == outer && (inner.size() == outer.size() || inner[outer.size()] == '.');
    };
    ChangeMark mark = ChangeMark::None;
    for (const core::AppliedChange& change : applied) {
        if (within(path, change.change.path)) {
            return ChangeMark::Changed;
        }
        if (within(change.change.path, path)) {
            mark = ChangeMark::Contains; // Keep looking: another change may target this node itself.
        }
    }
    return mark;
}

std::vector<PromptSection> PromptSections(const inference::Prompt& prompt)
{
    std::vector<PromptSection> sections;
    for (std::size_t i = 0; i < prompt.messages.size(); ++i) {
        const inference::Message& message = prompt.messages[i];
        const char* layer = i == 0                   ? "L0 engine rules, L1 world, L2 summary"
                            : i < prompt.stableCount ? "L3 transcript"
                                                     : "tail (T1-T4)";
        sections.push_back({std::format("#{} {} - {}", i, inference::RoleName(message.role), layer), message.content});
    }
    return sections;
}

std::string RawPrompt(const inference::Prompt& prompt)
{
    std::string text;
    for (const inference::Message& message : prompt.messages) {
        text += std::format("=== {} ===\n{}\n", inference::RoleName(message.role), message.content);
    }
    return text;
}

std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call)
{
    const std::string key(call);
    if (!timings.is_object() || !timings.contains(key)) {
        return std::nullopt;
    }
    try {
        const nlohmann::json& numbers = timings.at(key);
        return CallStats{numbers.at("prompt_tokens").get<int>(), numbers.at("reused").get<int>(),
                         numbers.at("evaluated").get<int>(),     numbers.at("generated").get<int>(),
                         numbers.at("prompt_ms").get<double>(),  numbers.at("generate_ms").get<double>(),
                         numbers.at("stopped_at_limit").get<bool>()};
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry '{}': {}", key, error.what()));
    }
}

std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history,
                                        const std::vector<core::Note>& notes)
{
    std::vector<RetrievalRow> rows;
    if (!timings.is_object() || !timings.contains("retrieval")) {
        return rows;
    }
    // A decimal number from a ref, or nullopt.
    auto number = [](std::string_view text) -> std::optional<int> {
        int value = 0;
        const auto [pNext, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || pNext != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    };
    try {
        for (const nlohmann::json& hit : timings.at("retrieval")) {
            RetrievalRow row;
            row.source = hit.at("source").get<std::string>();
            row.ref = hit.at("ref").get<std::string>();
            row.score = hit.at("score").get<double>();
            const std::string missing =
                std::format("turn telemetry names {} '{}', which the run does not hold", row.source, row.ref);
            if (row.source == "history") {
                // The ref is the turn number.
                const std::optional<int> turn = number(row.ref);
                const auto found = std::find_if(history.begin(), history.end(), [&](const core::TurnRecord& record) {
                    return turn && record.number == *turn;
                });
                if (found == history.end()) {
                    throw core::DataError(missing);
                }
                row.text = found->input;
            } else if (row.source == "notes") {
                // The ref is "<turn>.<index>": the index-th note that turn wrote.
                const std::size_t dot = row.ref.find('.');
                const std::optional<int> turn =
                    dot == std::string::npos ? std::nullopt : number(std::string_view(row.ref).substr(0, dot));
                const std::optional<int> index =
                    dot == std::string::npos ? std::nullopt : number(std::string_view(row.ref).substr(dot + 1));
                int seen = 0;
                bool found = false;
                for (const core::Note& note : notes) {
                    if (turn && index && note.turn == *turn && seen++ == *index) {
                        row.text = note.text;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw core::DataError(missing);
                }
            } else {
                row.text = row.ref; // A lore hit's ref is its subject.
            }
            rows.push_back(std::move(row));
        }
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'retrieval': {}", error.what()));
    }
    return rows;
}

std::string CheckText(const core::CheckOutcome& check)
{
    std::string rolls;
    for (const int roll : check.result.rolls) {
        rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
    }
    const int stat = check.result.statValue;
    const std::string statPart = stat == 0 ? "" : std::format(" {} {}", stat < 0 ? '-' : '+', std::abs(stat));
    return std::format("{} vs {}: rolled {}{} = {} against {}, {}", check.stat, check.difficulty, rolls, statPart,
                       check.result.total, check.result.target, check.result.success ? "success" : "failure");
}

} // namespace morpheus::app
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[inspect]"`
Expected: all five tests pass. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add src/app/CMakeLists.txt src/app/model/Inspect.h src/app/model/Inspect.cpp tests/app/InspectTests.cpp
git commit -m "Add the inspectors' view helpers" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

