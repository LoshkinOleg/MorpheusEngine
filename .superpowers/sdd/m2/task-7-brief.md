### Task 7: Context rendering: state view, mechanics line, transcript

**Files:**
- Create: `src/context/CMakeLists.txt`, `src/context/StateView.h`, `src/context/StateView.cpp`, `src/context/Transcript.h`, `src/context/Transcript.cpp`, `tests/context/RenderTests.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(src/context)` after `src/inference`), `tests/CMakeLists.txt` (link `morpheus_context`)

**Interfaces:**
- Consumes: `core::TurnRecord`, `core::CheckOutcome`, `core::AppliedChange`, `core::ChangeOp` (Task 2); `inference::Message` and `inference::Role` (Task 4).
- Produces (namespace `morpheus::context`):
  - `std::string RenderStateView(const nlohmann::json& state)`
  - `std::string RenderMechanicsLine(const core::TurnRecord& record)`
  - `std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns)`

- [ ] **Step 1: Write the failing test `tests/context/RenderTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "context/StateView.h"
#include "context/Transcript.h"

using namespace morpheus;

namespace {

core::TurnRecord TurnWithMechanics()
{
    core::TurnRecord record;
    record.number = 3;
    record.input = "I climb the hull.";
    record.narration = "You slip and hit the deck.";
    core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks = {check};
    record.applied = {
        {core::Change{core::ChangeOp::Set, "player.location", "deck", ""}, ""},
        {core::Change{core::ChangeOp::Add, "player.health", -3, ""}, "clamped to 0"},
        {core::Change{core::ChangeOp::Add, "player.conditions", "bruised", ""}, ""},
        {core::Change{core::ChangeOp::Remove, "player.inventory.rope", nullptr, ""}, ""},
        {core::Change{core::ChangeOp::Create, "npc:marta", nlohmann::json{{"type", "npc"}}, ""}, ""},
    };
    record.rejected = {{nlohmann::json{{"op", "explode"}}, "unknown op"}};
    return record;
}

} // namespace

TEST_CASE("RenderStateView prints one compact line per entity", "[context]")
{
    const nlohmann::json state = nlohmann::json::parse(R"({
        "player": {"type": "captain", "fields": {"health": 10, "location": "open desert", "conditions": [],
                                                  "inventory": {"pistol": 1, "compass": 1}}},
        "crawler": {"type": "crawler", "fields": {"crew": 20, "vehicles": {"sand_bike": 2}, "morale": "steady"}}
    })");
    CHECK(context::RenderStateView(state) ==
          "crawler (crawler): crew 20, morale \"steady\", vehicles {sand_bike 2}\n"
          "player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location \"open desert\"\n");
}

TEST_CASE("RenderMechanicsLine summarizes checks and applied changes", "[context]")
{
    CHECK(context::RenderMechanicsLine(TurnWithMechanics()) ==
          "[agility vs hard: 14/16 failure; player.location = \"deck\"; player.health -3 (clamped to 0); "
          "player.conditions +bruised; player.inventory.rope removed; npc:marta created]");

    core::TurnRecord quiet;
    quiet.input = "I wait.";
    CHECK(context::RenderMechanicsLine(quiet).empty());
}

TEST_CASE("RenderTranscript turns each turn into a user and an assistant message", "[context]")
{
    core::TurnRecord quiet;
    quiet.input = "I wait.";
    quiet.narration = "Nothing happens.";

    const std::vector<inference::Message> messages = context::RenderTranscript({TurnWithMechanics(), quiet});
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].role == inference::Role::User);
    CHECK(messages[0].content == "I climb the hull.\n" + context::RenderMechanicsLine(TurnWithMechanics()));
    CHECK(messages[1].role == inference::Role::Assistant);
    CHECK(messages[1].content == "You slip and hit the deck.");
    CHECK(messages[2].content == "I wait."); // No mechanics: no extra line.
    CHECK(messages[3].content == "Nothing happens.");
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'context/StateView.h'`.

- [ ] **Step 3: Create the headers**

`src/context/StateView.h`:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::context {

// The state view for the volatile tail (T1): one line per entity, e.g.
//   player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location "open desert"
// Entities and fields appear in name order (state JSON objects are sorted), so the text is deterministic.
// Numbers are bare, strings and enum values are quoted, list items are bare, counters print "item count".
// Throws ContractError for a state that is not shaped like validated state.
std::string RenderStateView(const nlohmann::json& state);

} // namespace morpheus::context
```

`src/context/Transcript.h`:

```cpp
#pragma once

#include <string>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::context {

// One line recording what the engine did in a turn, for the transcript (L3):
//   [agility vs hard: 14/16 failure; player.location = "deck"; player.health -3 (clamped to 0); npc:marta created]
// set: "path = value"; add: "path +N" / "path -N" / "path +item"; remove: "path -item" or "path removed";
// create: "id created"; an engine note follows in parentheses. Rejected changes are left out.
// Returns "" when the turn had no checks and no applied changes.
std::string RenderMechanicsLine(const core::TurnRecord& record);

// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one)
// and an assistant message (the narration).
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns);

} // namespace morpheus::context
```

- [ ] **Step 4: Create the implementations and CMake**

`src/context/StateView.cpp`:

```cpp
#include "context/StateView.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderStateView(const nlohmann::json& state)
{
    MORPHEUS_REQUIRE(state.is_object(), "RenderStateView: state must be an object");

    std::string text;
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const nlohmann::json& body = entity.value();
        MORPHEUS_REQUIRE(body.is_object() && body.contains("type") && body.contains("fields"),
                         std::format("RenderStateView: entity '{}' has no type or fields", entity.key()));
        text += std::format("{} ({}):", entity.key(), body.at("type").get<std::string>());

        const nlohmann::json& fields = body.at("fields");
        bool firstField = true;
        for (auto field = fields.begin(); field != fields.end(); ++field) {
            text += firstField ? " " : ", ";
            firstField = false;
            text += field.key();
            text += ' ';

            const nlohmann::json& value = field.value();
            if (value.is_array()) {
                // List: bare items.
                text += '[';
                for (std::size_t i = 0; i < value.size(); ++i) {
                    text += (i == 0 ? "" : ", ") + value[i].get<std::string>();
                }
                text += ']';
            } else if (value.is_object()) {
                // Counter: "item count" pairs.
                text += '{';
                bool firstItem = true;
                for (auto item = value.begin(); item != value.end(); ++item) {
                    text += std::format("{}{} {}", firstItem ? "" : ", ", item.key(), item.value().dump());
                    firstItem = false;
                }
                text += '}';
            } else {
                text += value.dump(); // Numbers bare, strings quoted and escaped.
            }
        }
        text += '\n';
    }
    return text;
}

} // namespace morpheus::context
```

`src/context/Transcript.cpp`:

```cpp
#include "context/Transcript.h"

#include <format>

namespace morpheus::context {

std::string RenderMechanicsLine(const core::TurnRecord& record)
{
    std::vector<std::string> parts;
    for (const core::CheckOutcome& check : record.checks) {
        parts.push_back(std::format("{} vs {}: {}/{} {}", check.stat, check.difficulty, check.result.total,
                                    check.result.target, check.result.success ? "success" : "failure"));
    }
    for (const core::AppliedChange& applied : record.applied) {
        const core::Change& change = applied.change;
        std::string part;
        switch (change.op) {
        case core::ChangeOp::Set:
            part = std::format("{} = {}", change.path, change.value.dump());
            break;
        case core::ChangeOp::Add:
            // Numeric add (int field or counter item) or a list item.
            part = change.value.is_number_integer() ? std::format("{} {:+}", change.path, change.value.get<long long>())
                                                    : std::format("{} +{}", change.path, change.value.get<std::string>());
            break;
        case core::ChangeOp::Remove:
            // A list item names what was removed; a counter item is removed whole.
            part = change.value.is_string() ? std::format("{} -{}", change.path, change.value.get<std::string>())
                                            : std::format("{} removed", change.path);
            break;
        case core::ChangeOp::Create:
            part = std::format("{} created", change.path);
            break;
        }
        if (!applied.note.empty()) {
            part += std::format(" ({})", applied.note);
        }
        parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        return {};
    }
    std::string line = "[";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        line += (i == 0 ? "" : "; ") + parts[i];
    }
    return line + "]";
}

std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns)
{
    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (const core::TurnRecord& turn : turns) {
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        messages.push_back({inference::Role::Assistant, turn.narration});
    }
    return messages;
}

} // namespace morpheus::context
```

`src/context/CMakeLists.txt` (PromptBuilder.cpp is added in Task 8):

```cmake
add_library(morpheus_context STATIC
    StateView.cpp
    Transcript.cpp
)
target_include_directories(morpheus_context PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_context PUBLIC morpheus_core morpheus_inference)
morpheus_target_defaults(morpheus_context)
```

Then:
- Add `add_subdirectory(src/context)` after `add_subdirectory(src/inference)` in the root `CMakeLists.txt`.
- Add `morpheus_context` to `morpheus_tests`'s `target_link_libraries` list.

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[context]`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/context tests
git commit -m "Render state view, mechanics lines and transcript" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

