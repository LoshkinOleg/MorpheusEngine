### Task 13: Replay: recording, fixtures and the replay test

**Files:**
- Create: `src/turn/Replay.h`, `src/turn/Replay.cpp`, `tests/turn/ReplayTests.cpp`, `tests/data/replays/mini-undo-regenerate.json`
- Modify: `src/turn/CMakeLists.txt` (add `Replay.cpp`)

**Interfaces:**
- Consumes:
  - `Session`, `core::CheckOutcomeToJson`, `AppliedChangeToJson`, `RejectedChangeToJson`
  - `SUMMARIZE_INSTRUCTION` and `CONDENSE_INSTRUCTION`
- Produces:
  - `turn::CallKindOf(const inference::Prompt&, const inference::GenerationSpec&) -> core::CallKind`
  - `turn::CallKindName(core::CallKind) -> std::string`
  - `struct turn::ReplayStep { enum class Action { Input, Undo, Regenerate }; Action action; std::string input; }`
  - `class turn::RecordingBackend(inference::LlmBackend& inner)`, with `Replies()`
  - `turn::TurnResultJson(const core::TurnRecord&) -> nlohmann::json` (what a replay compares per turn)
  - `turn::MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode, const std::vector<ReplayStep>&, const RecordingBackend&, const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState) -> nlohmann::json`

Fixture format (`tests/data/replays/*.json`):

```
{"world": "<folder relative to the repo root>", "seed": N, "memory_mode": "engine" | "assisted",
 "steps": [{"input": "..."} | {"undo": true} | {"regenerate": true}],
 "replies": {"lookup": [...], "resolve": [...], "narrate": [...], "summarize": [...]},
 "expected": {"turns": [{"number", "input", "checks", "applied", "rejected", "narration"}], "final_state": {...}}}
```

Replies are recorded as strings, exactly as the model wrote them. A hand-written fixture may give a resolve reply as a JSON object, which is replayed as its `dump()`.

Replaying does not reproduce prompts: retrieval and fold timing depend on the embedding model and the tokenizer. It does reproduce every committed result, because those depend only on the replies, the seed and the engine:
- checks and rolls;
- applied and rejected changes;
- narration and state.

When fold timing differs, a replay that runs out of recorded summaries gets "Summary.".

- [ ] **Step 1: Write the fixture and the failing test**

Create `tests/data/replays/mini-undo-regenerate.json`. It plays two turns, undoes the second, plays another, and regenerates that one. The regenerated turn's resolve reply removes the lantern instead of the third reply's rejected oil change.

```json
{
  "world": "tests/data/worlds/mini",
  "seed": 7,
  "memory_mode": "engine",
  "steps": [
    {"input": "I walk to the lighthouse."},
    {"input": "I climb the stairs."},
    {"undo": true},
    {"input": "I light the lantern."},
    {"regenerate": true}
  ],
  "replies": {
    "resolve": [
      {"intent": "walk", "checks": [], "changes": [{"op": "set", "path": "player.location", "value": "lighthouse", "reason": "walks there"}]},
      {"intent": "climb", "checks": [], "changes": [{"op": "set", "path": "player.location", "value": "stairs", "reason": "climbs"}]},
      {"intent": "light", "checks": [], "changes": [
        {"op": "add", "path": "player.inventory.oil", "value": -1, "reason": "burns oil"},
        {"op": "set", "path": "player.location", "value": "lamp room", "reason": "goes up"}]},
      {"intent": "light", "checks": [], "changes": [{"op": "remove", "path": "player.inventory.lantern", "reason": "the lantern is set in place"}]}
    ],
    "narrate": [
      "You walk to the lighthouse.",
      "You climb the stairs.",
      "The oil is gone; you reach the lamp room.",
      "You set the lantern in its cradle."
    ]
  },
  "expected": {
    "turns": [
      {"number": 1, "input": "I walk to the lighthouse.", "checks": [],
       "applied": [{"change": {"op": "set", "path": "player.location", "value": "lighthouse", "reason": "walks there"}, "note": ""}],
       "rejected": [], "narration": "You walk to the lighthouse."},
      {"number": 2, "input": "I light the lantern.", "checks": [],
       "applied": [{"change": {"op": "remove", "path": "player.inventory.lantern", "reason": "the lantern is set in place"}, "note": ""}],
       "rejected": [], "narration": "You set the lantern in its cradle."}
    ],
    "final_state": {
      "player": {"type": "character", "fields": {"health": 10, "agility": 3, "location": "lighthouse", "inventory": {}}}
    }
  }
}
```

Create `tests/turn/ReplayTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/Replay.h"
#include "turn/Session.h"

using namespace morpheus;
namespace fs = std::filesystem;

namespace {

core::Settings ReplaySettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {512, 768, 512, 2048, 1024, 4096, 2048, 768}; // As settings.example.json.
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

nlohmann::json ReadJson(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    return nlohmann::json::parse(stream);
}

} // namespace

TEST_CASE("Recorded sessions replay to the same turns and state", "[replay]")
{
    for (const fs::directory_entry& entry : fs::directory_iterator(test::TestDataDir() / "replays")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        DYNAMIC_SECTION(entry.path().filename().string())
        {
            const nlohmann::json fixture = ReadJson(entry.path());
            std::map<std::string, std::deque<std::string>> replies;
            for (const auto& [kind, list] : fixture.at("replies").items()) {
                for (const nlohmann::json& reply : list) {
                    replies[kind].push_back(reply.is_string() ? reply.get<std::string>() : reply.dump());
                }
            }
            test::ScriptedBackend backend(16384, [&replies](const inference::Prompt& prompt,
                                                            const inference::GenerationSpec& spec) {
                const core::CallKind kind = turn::CallKindOf(prompt, spec);
                std::deque<std::string>& queue = replies[turn::CallKindName(kind)];
                if (queue.empty()) {
                    // Fold timing depends on the tokenizer, so a replay may fold where the recording did not.
                    MORPHEUS_REQUIRE(kind == core::CallKind::Summarize,
                                     "replay ran out of " + turn::CallKindName(kind) + " replies");
                    return std::string("Summary.");
                }
                std::string reply = std::move(queue.front());
                queue.pop_front();
                return reply;
            });

            core::Settings settings = ReplaySettings();
            settings.memoryMode = core::ParseMemoryMode(fixture.at("memory_mode").get<std::string>());
            turn::Session session = turn::Session::Start(
                core::LoadWorld(test::RepoDir() / fixture.at("world").get<std::string>()), settings,
                test::FreshTempDir("replay_" + entry.path().stem().string()) / "run.db",
                fixture.at("seed").get<std::uint64_t>(), backend);

            for (const nlohmann::json& step : fixture.at("steps")) {
                if (step.contains("input")) {
                    REQUIRE(session.RunTurn(step.at("input").get<std::string>(), {}, {}));
                    REQUIRE(session.Maintain({}));
                } else if (step.contains("undo")) {
                    session.Undo();
                } else {
                    REQUIRE(session.Regenerate({}, {}));
                    REQUIRE(session.Maintain({}));
                }
            }

            const nlohmann::json& expected = fixture.at("expected");
            const std::vector<core::TurnRecord> history = session.History();
            REQUIRE(history.size() == expected.at("turns").size());
            for (std::size_t i = 0; i < history.size(); ++i) {
                CHECK(turn::TurnResultJson(history[i]) == expected.at("turns")[i]);
            }
            CHECK(session.State() == expected.at("final_state"));
        }
    }
}

TEST_CASE("RecordingBackend records each call's text by kind and MakeReplayFixture writes the format", "[replay]")
{
    test::ScriptedBackend inner(16384, [](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.empty() ? std::string("You walk.") : std::string(R"({"intent":"walk","checks":[],"changes":[]})");
    });
    turn::RecordingBackend recording(inner);
    turn::Session session = turn::Session::Start(core::LoadWorld(test::TestDataDir() / "worlds" / "mini"),
                                                 ReplaySettings(), test::FreshTempDir("replay_record") / "run.db", 9,
                                                 recording);
    REQUIRE(session.RunTurn("I walk.", {}, {}));

    CHECK(recording.Replies().at(core::CallKind::Resolve) == std::vector<std::string>{R"({"intent":"walk","checks":[],"changes":[]})"});
    CHECK(recording.Replies().at(core::CallKind::Narrate) == std::vector<std::string>{"You walk."});

    const nlohmann::json fixture = turn::MakeReplayFixture("tests/data/worlds/mini", 9, core::MemoryMode::Engine,
                                                           {{turn::ReplayStep::Action::Input, "I walk."}}, recording,
                                                           session.History(), session.State());
    CHECK(fixture.at("world") == "tests/data/worlds/mini");
    CHECK(fixture.at("seed") == 9);
    CHECK(fixture.at("memory_mode") == "engine");
    CHECK(fixture.at("steps") == nlohmann::json::parse(R"([{"input": "I walk."}])"));
    CHECK(fixture.at("replies").at("narrate") == nlohmann::json::array({"You walk."}));
    CHECK(fixture.at("expected").at("turns").at(0).at("narration") == "You walk.");
    CHECK(fixture.at("expected").at("final_state") == session.State());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Replay.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/Replay.h`:

```cpp
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Dice.h"
#include "core/MemoryMode.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::turn {

// Which call a completion answers, told apart the way the session makes them: the lookup grammar names "lookups",
// any other grammar is the resolve call's, and a grammar-free call ending with the summarize or condense
// instruction is a summary; the rest is narration.
core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec);

// Fixture spelling of a call kind: "lookup", "resolve", "narrate" or "summarize".
std::string CallKindName(core::CallKind kind);

// One player command in a replay fixture.
struct ReplayStep {
    enum class Action { Input, Undo, Regenerate };
    Action action = Action::Input;
    std::string input = {}; // Input only.
};

// Wraps a backend during play and records what a replay fixture needs (architecture spec, "Testing"): the text of
// every completed call, by kind, in order. Cancelled calls are not recorded. Everything else is forwarded.
class RecordingBackend final : public inference::LlmBackend {
private:
    inference::LlmBackend* _pInner = nullptr; // Not owned.
    std::map<core::CallKind, std::vector<std::string>> _replies = {};

public:
    explicit RecordingBackend(inference::LlmBackend& inner);

    const std::map<core::CallKind, std::vector<std::string>>& Replies() const;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
};

// What a replay checks of one committed turn: {"number", "input", "checks", "applied", "rejected", "narration"},
// in the store's JSON forms.
nlohmann::json TurnResultJson(const core::TurnRecord& record);

// A replay fixture (format in tests/data/replays and the M3 plan): the world folder (relative to the repo root),
// seed, memory mode, the steps played, every reply by kind, and what the session committed.
nlohmann::json MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode,
                                 const std::vector<ReplayStep>& steps, const RecordingBackend& recording,
                                 const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState);

} // namespace morpheus::turn
```

Create `src/turn/Replay.cpp`:

```cpp
#include "turn/Replay.h"

#include "core/Errors.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)
{
    if (!spec.grammar.empty()) {
        return spec.grammar.find("lookups") != std::string::npos ? core::CallKind::Lookup : core::CallKind::Resolve;
    }
    const std::string& last = prompt.messages.empty() ? std::string() : prompt.messages.back().content;
    if (last == SUMMARIZE_INSTRUCTION || last == CONDENSE_INSTRUCTION) {
        return core::CallKind::Summarize;
    }
    return core::CallKind::Narrate;
}

std::string CallKindName(core::CallKind kind)
{
    switch (kind) {
    case core::CallKind::Lookup:
        return "lookup";
    case core::CallKind::Resolve:
        return "resolve";
    case core::CallKind::Narrate:
        return "narrate";
    case core::CallKind::Summarize:
        return "summarize";
    }
    throw core::ContractError("CallKindName: unhandled CallKind");
}

RecordingBackend::RecordingBackend(inference::LlmBackend& inner)
    : _pInner(&inner)
{
}

const std::map<core::CallKind, std::vector<std::string>>& RecordingBackend::Replies() const
{
    return _replies;
}

int RecordingBackend::CountTokens(std::string_view text)
{
    return _pInner->CountTokens(text);
}

int RecordingBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pInner->CountPromptTokens(prompt);
}

int RecordingBackend::ContextSize() const
{
    return _pInner->ContextSize();
}

inference::Completion RecordingBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                 const inference::TokenCallback& onToken, std::stop_token stop)
{
    inference::Completion completion = _pInner->Complete(prompt, spec, onToken, stop);
    if (!completion.cancelled) {
        _replies[CallKindOf(prompt, spec)].push_back(completion.text);
    }
    return completion;
}

std::vector<std::vector<float>> RecordingBackend::Embed(const std::vector<std::string>& texts,
                                                        inference::EmbedPurpose purpose)
{
    return _pInner->Embed(texts, purpose);
}

std::string RecordingBackend::EmbeddingModelId() const
{
    return _pInner->EmbeddingModelId();
}

std::vector<std::uint8_t> RecordingBackend::SaveState()
{
    return _pInner->SaveState();
}

void RecordingBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pInner->RestoreState(state);
}

nlohmann::json TurnResultJson(const core::TurnRecord& record)
{
    nlohmann::json checks = nlohmann::json::array();
    for (const core::CheckOutcome& check : record.checks) {
        checks.push_back(core::CheckOutcomeToJson(check));
    }
    nlohmann::json applied = nlohmann::json::array();
    for (const core::AppliedChange& change : record.applied) {
        applied.push_back(core::AppliedChangeToJson(change));
    }
    nlohmann::json rejected = nlohmann::json::array();
    for (const core::RejectedChange& change : record.rejected) {
        rejected.push_back(core::RejectedChangeToJson(change));
    }
    return {{"number", record.number}, {"input", record.input}, {"checks", checks},
            {"applied", applied},      {"rejected", rejected},  {"narration", record.narration}};
}

nlohmann::json MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode,
                                 const std::vector<ReplayStep>& steps, const RecordingBackend& recording,
                                 const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState)
{
    nlohmann::json stepsJson = nlohmann::json::array();
    for (const ReplayStep& step : steps) {
        switch (step.action) {
        case ReplayStep::Action::Input:
            stepsJson.push_back({{"input", step.input}});
            break;
        case ReplayStep::Action::Undo:
            stepsJson.push_back({{"undo", true}});
            break;
        case ReplayStep::Action::Regenerate:
            stepsJson.push_back({{"regenerate", true}});
            break;
        }
    }
    nlohmann::json replies = nlohmann::json::object();
    for (const auto& [kind, texts] : recording.Replies()) {
        replies[CallKindName(kind)] = texts;
    }
    nlohmann::json turns = nlohmann::json::array();
    for (const core::TurnRecord& record : history) {
        turns.push_back(TurnResultJson(record));
    }
    return {{"world", worldFolder},
            {"seed", seed},
            {"memory_mode", mode == core::MemoryMode::Engine ? "engine" : "assisted"},
            {"steps", stepsJson},
            {"replies", replies},
            {"expected", {{"turns", turns}, {"final_state", finalState}}}};
}

} // namespace morpheus::turn
```

`CallKindOf` binds a `const std::string&` to a temporary when the prompt is empty. Use a local `std::string last` copy, or check `prompt.messages.empty()` first, whichever reads cleaner under `/W4`.

Add `Replay.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/turn/ReplayTests.cpp tests/data/replays
git commit -m "Add replay recording, fixtures and the replay test" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

