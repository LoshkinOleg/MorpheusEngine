### Task 10: Session: one run and its turn loop (engine mode)

**Files:**
- Create: `src/turn/Session.h`, `src/turn/Session.cpp`, `tests/turn/SessionTests.cpp`
- Modify: `src/turn/CMakeLists.txt` (add `Session.cpp`)

**Interfaces:**
- Consumes: everything from Tasks 1-9. In particular:
  - `context::AssembleResolvePrompt`, `context::ExtendForNarrate`
  - `BuildResolveGrammar`, `ParseResolveOutput`, `ApplyProposal`, `RenderNarrateMessage`
  - `MemoryIndex`, `SummarizeTurns`, `CondenseSummaries`
  - `core::SamplingSeed`, `inference::SamplingFrom`, `core::SettingsToJson`
  - `store::RunStore::{Create, Open, ReplaceSummaries, TruncateAfter}`
- Produces:
  - `class turn::TurnError : public std::runtime_error`
  - The constants `RETRIEVAL_MAX_HITS`, `LOOKUP_MAX_HITS`, `LOOKUP_MAX_TOKENS`, `FOLD_SUMMARY_DIVISOR` and `CONDENSE_SUMMARY_DIVISOR`
  - `struct turn::TurnCallbacks { std::function<void(const core::TurnRecord&)> onResolved; inference::TokenCallback onNarrationToken; }`
  - `class turn::Session`:
    - `static Start(core::World, core::Settings, const std::filesystem::path& file, std::uint64_t seed, inference::LlmBackend&)`
    - `static Load(core::World, core::Settings, const std::filesystem::path& file, inference::LlmBackend&)`
    - `RunTurn(std::string_view input, const TurnCallbacks&, std::stop_token) -> std::optional<core::TurnRecord>`
    - `Undo()`
    - `Regenerate(const TurnCallbacks&, std::stop_token) -> std::optional<core::TurnRecord>`
    - `Maintain(std::stop_token) -> bool`
    - `Meta()`, `WorldChanged()`, `State()`, `History()`, `Summaries()`, `LastAssembly()`

- [ ] **Step 1: Write the failing tests**

Create `tests/turn/SessionTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/Session.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const std::string MOVE = R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";
const std::string CLIMB = R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall",)"
                          R"("on_success":[],"on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],"changes":[]})";

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

core::Settings TestSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// Answers the way the model would, by call: resolve calls (their grammar has "intent") get `resolve`, summary calls
// get "Summary.", and everything else is narration.
test::ScriptedBackend::Responder Responder(std::string resolve, std::string narration = "You walk on.")
{
    return [resolve, narration](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("intent") != std::string::npos) {
            return resolve;
        }
        const std::string& last = prompt.messages.back().content;
        if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
            return std::string("Summary.");
        }
        return narration;
    };
}

fs::path NewRunFile(const std::string& name)
{
    return test::FreshTempDir(name) / "run.db";
}

} // namespace

TEST_CASE("A turn resolves, applies, narrates and commits", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_turn"), 42, backend);

    std::vector<std::string> events;
    turn::TurnCallbacks callbacks;
    callbacks.onResolved = [&](const core::TurnRecord& record) {
        events.push_back("resolved " + record.applied.at(0).change.path);
    };
    callbacks.onNarrationToken = [&](std::string_view) {
        if (events.back() != "token") {
            events.push_back("token");
        }
    };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->number == 1);
    CHECK(record->attempt == 0);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(events == std::vector<std::string>{"resolved player.location", "token"});
    CHECK(record->timings.contains("resolve"));
    CHECK(record->timings.contains("narrate"));
    CHECK(session.State().at("player").at("fields").at("location") == "lighthouse");
    CHECK(session.History().size() == 1);
    CHECK(session.Meta().modelId == "chat");
}

TEST_CASE("The narrate prompt extends the resolve prompt", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_prompts"), 42, backend);
    REQUIRE(session.RunTurn("I walk to the lighthouse.", {}, {}));

    const auto& calls = backend.Calls();
    REQUIRE(calls.size() == 2);
    const inference::Prompt& resolve = calls[0].prompt;
    const inference::Prompt& narrate = calls[1].prompt;
    CHECK_THAT(calls[0].spec.grammar, ContainsSubstring("root ::="));
    CHECK(calls[1].spec.grammar.empty());
    CHECK_THAT(resolve.messages.front().content, ContainsSubstring("# Engine"));
    CHECK_THAT(resolve.messages.front().content, ContainsSubstring("# Game reference"));
    CHECK_THAT(resolve.messages.back().content, ContainsSubstring(std::string(turn::RESOLVE_INSTRUCTION)));
    REQUIRE(narrate.messages.size() == resolve.messages.size() + 2);
    for (std::size_t i = 0; i < resolve.messages.size(); ++i) {
        CHECK(narrate.messages[i].content == resolve.messages[i].content);
    }
    CHECK(narrate.messages[resolve.messages.size()].content == MOVE);
    CHECK_THAT(narrate.messages.back().content, ContainsSubstring("player.location = \"lighthouse\""));
}

TEST_CASE("Unreadable resolve output still narrates, with nothing changed", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(R"({"intent":"walk","checks":[)", "You hesitate."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_unreadable"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk.", {}, {});
    REQUIRE(record);
    CHECK(record->applied.empty());
    CHECK(record->resolve.contains("unparsed"));
    CHECK(record->narration == "You hesitate.");
    CHECK_THAT(backend.Calls().at(1).prompt.messages.back().content, ContainsSubstring("could not be resolved"));
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("Cancelling a turn commits nothing", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk a long way."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_cancel"), 42, backend);
    std::stop_source stop;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationToken = [&](std::string_view) { stop.request_stop(); };
    CHECK_FALSE(session.RunTurn("I walk.", callbacks, stop.get_token()));
    CHECK(session.History().empty());
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("Undo removes the latest turn", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_undo"), 42, backend);
    CHECK_THROWS_AS(session.Undo(), turn::TurnError);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    session.Undo();
    CHECK(session.History().empty());
    CHECK(session.State() == MiniWorld().initialState);
}

TEST_CASE("Regenerate keeps the rolls and samples new text", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(CLIMB, "You climb."));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_regenerate"), 42, backend);
    CHECK_THROWS_AS(session.Regenerate({}, {}), turn::TurnError);
    REQUIRE(session.RunTurn("I climb the cliff.", {}, {}));
    const core::TurnRecord first = session.History().at(0);

    const std::optional<core::TurnRecord> second = session.Regenerate({}, {});
    REQUIRE(second);
    CHECK(second->number == 1);
    CHECK(second->attempt == 1);
    CHECK(second->input == "I climb the cliff.");
    REQUIRE(second->checks.size() == 1);
    CHECK(second->checks[0].result.rolls == first.checks[0].result.rolls);
    const auto& calls = backend.Calls(); // resolve, narrate, resolve, narrate
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].spec.sampling.seed != calls[0].spec.sampling.seed);
    CHECK(calls[3].spec.sampling.seed != calls[1].spec.sampling.seed);
    CHECK(session.History().size() == 1);

    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Regenerate({}, stop.get_token()));
    REQUIRE(session.History().size() == 1);
    CHECK(session.History()[0].attempt == 1); // The cancelled attempt put the previous turn back.
}

TEST_CASE("Folding keeps every prompt within the context", "[session]")
{
    core::Settings settings = TestSettings();
    // FakeBackend counts 1 token per 4 bytes: the system message is about 700 tokens, so L3 gets about 1000, while
    // each turn adds about 520 (a 2000-byte narration). Folding must start by the third turn.
    settings.budget = {64, 600, 200, 400, 1024, 1024, 512, 100};
    test::ScriptedBackend backend(3000, Responder(MOVE, std::string(2000, 'w')));
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_folding"), 42, backend);

    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
        CHECK(session.LastAssembly().Fits());
        REQUIRE(session.Maintain({}));
    }
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK_THAT(session.LastAssembly().prompt.messages.front().content, ContainsSubstring("# Story so far"));
    CHECK(session.LastAssembly().prompt.messages.size() < 2 + 2 * 6);
}

TEST_CASE("Load reopens a run and checks it against the world", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    const fs::path file = NewRunFile("session_load");
    {
        turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), file, 42, backend);
        REQUIRE(session.RunTurn("I walk.", {}, {}));
    }
    turn::Session loaded = turn::Session::Load(MiniWorld(), TestSettings(), file, backend);
    CHECK_FALSE(loaded.WorldChanged());
    CHECK(loaded.History().size() == 1);
    CHECK(loaded.State().at("player").at("fields").at("location") == "lighthouse");

    // The same world with an edited rules.md opens, flagged.
    const fs::path editedFolder = test::FreshTempDir("session_load_edited") / "mini";
    fs::copy(test::TestDataDir() / "worlds" / "mini", editedFolder, fs::copy_options::recursive);
    test::WriteFile(editedFolder / "rules.md", "You are the GM of an edited test world.\n");
    CHECK(turn::Session::Load(core::LoadWorld(editedFolder), TestSettings(), file, backend).WorldChanged());

    const core::World other = core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");
    CHECK_THROWS_AS(turn::Session::Load(other, TestSettings(), file, backend), core::DataError);
}

TEST_CASE("Player input is cleaned and must not be empty", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_input"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("\x01  I walk.\x1E \n", {}, {});
    REQUIRE(record);
    CHECK(record->input == "I walk.");
    CHECK_THROWS_AS(session.RunTurn(" \x02 ", {}, {}), turn::TurnError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Session.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/Session.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// A turn that cannot run as asked: nothing to undo or regenerate, an empty input, or an input too long for the
// tail allowance. Nothing changed; the message is for the player.
class TurnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr std::size_t RETRIEVAL_MAX_HITS = 6; // Retrieval hits in the tail (T2), before retrieval_cap applies.
constexpr std::size_t LOOKUP_MAX_HITS = 2;    // Hits each assisted-mode lookup adds.
constexpr int LOOKUP_MAX_TOKENS = 192;        // Generation cap for the lookup call.
constexpr int FOLD_SUMMARY_DIVISOR = 4;       // A fold summary gets summary_cap / 4 tokens: several fit before a condense.
constexpr int CONDENSE_SUMMARY_DIVISOR = 2;   // A condensed summary gets summary_cap / 2 tokens, so it fits its cap.

struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    inference::TokenCallback onNarrationToken = {};
};

// One open run: its world, settings, database and memory index, and the turn loop (architecture spec, "Turn flow").
// Every backend call happens inside these methods, on the caller's thread (the engine worker). Not thread-safe.
class Session {
private:
    core::World _world;     // Set by the constructor.
    core::Settings _settings;
    core::MemoryMode _mode = core::MemoryMode::Engine;
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index;     // Set by the constructor.
    context::Assembly _lastAssembly = {};
    bool _worldChanged = false;

public:
    // Starts a new run in `file` from the world's initial state, with `seed` for dice and sampling, and indexes the
    // lore. Throws StoreError if the file exists, and DataError if lore cannot be embedded (then no file is left).
    static Session Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                         std::uint64_t seed, inference::LlmBackend& backend);

    // Opens a run of `world`. Throws DataError if the run belongs to another world or its latest state no longer
    // validates against the world's schema. Edited world files set WorldChanged(); edited lore is re-embedded.
    static Session Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                        inference::LlmBackend& backend);

    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, commit. Control characters other
    // than newline and tab are stripped from `input` and it is trimmed. Returns the committed record, or nullopt
    // when `stop` cancelled the turn (nothing is committed). Throws TurnError for an empty input or one that does
    // not fit, and InferenceError from the backend (nothing is committed).
    std::optional<core::TurnRecord> RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                            std::stop_token stop);

    // Removes the latest turn. Throws TurnError when there is none.
    void Undo();

    // Plays the latest turn's input again as a new attempt: same dice, newly sampled text. If it is cancelled or
    // fails, the previous turn is put back. Throws TurnError when there is no turn.
    std::optional<core::TurnRecord> Regenerate(const TurnCallbacks& callbacks, std::stop_token stop);

    // Between-turn upkeep: folds or condenses while the transcript or the summary is over budget, then indexes
    // folded turns and notes. Returns false if `stop` cancelled it (it can simply run again later).
    bool Maintain(std::stop_token stop);

    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    nlohmann::json State();                  // After the latest turn.
    std::vector<core::TurnRecord> History(); // Every committed turn, oldest first.
    std::vector<core::Summary> Summaries();
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the context inspector.

private:
    Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
            inference::LlmBackend& backend);

    // RunTurn and Regenerate: plays turn LatestTurn()+1 for a cleaned input.
    std::optional<core::TurnRecord> PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                             std::stop_token stop);

    // The layers for one call: summaries and unfolded transcript from the store, then the given tail parts.
    context::ContextInputs Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,
                                  const std::string& input, std::string_view instruction);

    // Assembles a prompt that fits, shedding hits, condensing and folding as needed (see the loop). nullopt when a
    // memory call was cancelled.
    std::optional<context::Assembly> AssembleFitting(const nlohmann::json& state, std::vector<RetrievalHit>& hits,
                                                     const std::string& input, std::string_view instruction, int turn,
                                                     int attempt, std::stop_token stop);

    // Folds the oldest half (at least one) of `transcript` into a new summary. False when cancelled.
    bool FoldOldest(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& transcript, int turn,
                    int attempt, std::stop_token stop);

    // Replaces every summary with one condensed summary. False when cancelled.
    bool Condense(const inference::Message& systemMessage, int turn, int attempt, std::stop_token stop);

    // The last turn covered by a summary, 0 when none.
    int FoldedThrough();
};

} // namespace morpheus::turn
```

Create `src/turn/Session.cpp`:

```cpp
#include "turn/Session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/Folding.h"
#include "turn/Resolve.h"
#include "turn/ResolveGrammar.h"

namespace morpheus::turn {

namespace {

// Automatic retrieval searches all of memory.
constexpr std::array<store::EmbeddingKind, 3> ALL_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                             store::EmbeddingKind::Note};

// Stands in for the next input when maintenance measures the stable layers between turns.
constexpr std::string_view MAINTENANCE_INPUT = "(the player's next action)";

// One model call's numbers for the turn telemetry (TurnRecord::timings).
nlohmann::json Telemetry(const inference::Completion& completion)
{
    return {{"prompt_tokens", completion.promptTokens},  {"reused", completion.tokensReused},
            {"evaluated", completion.tokensEvaluated},   {"generated", completion.tokensGenerated},
            {"prompt_ms", completion.promptMs},          {"generate_ms", completion.generateMs},
            {"stopped_at_limit", completion.stoppedAtLimit}};
}

} // namespace

Session::Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
                 inference::LlmBackend& backend)
    : _world(std::move(world))
    , _settings(std::move(settings))
    , _store(std::move(store))
    , _pBackend(&backend)
    , _index(std::move(index))
{
    _mode = _settings.memoryMode.value_or(_world.defaultMemoryMode);
}

Session Session::Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                       std::uint64_t seed, inference::LlmBackend& backend)
{
    store::RunMeta meta;
    meta.worldId = world.id;
    meta.worldHash = world.contentHash;
    meta.modelId = core::PathText(settings.chatModel.stem());
    meta.rngSeed = seed;
    meta.settings = core::SettingsToJson(settings);
    store::RunStore store = store::RunStore::Create(file, meta, world.initialState);

    MemoryIndex index(settings.embeddingDocumentPrefix);
    try {
        index.SyncLore(world, store, backend);
    } catch (...) {
        // The run never started: close the database (a moved-from store holds no handle), then remove the file.
        { store::RunStore closing = std::move(store); }
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        throw;
    }
    return Session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
}

Session Session::Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                      inference::LlmBackend& backend)
{
    store::RunStore store = store::RunStore::Open(file);
    if (store.Meta().worldId != world.id) {
        throw core::DataError(std::format("run '{}' belongs to world '{}', not '{}'", core::PathText(file),
                                          store.Meta().worldId, world.id));
    }
    try {
        world.schema.ValidateState(store.StateAt(store.LatestTurn()));
    } catch (const core::DataError& error) {
        throw core::DataError(std::format("run '{}': its latest state no longer fits world '{}': {}",
                                          core::PathText(file), world.id, error.what()));
    }

    MemoryIndex index(settings.embeddingDocumentPrefix);
    index.SyncLore(world, store, backend);
    const bool worldChanged = store.Meta().worldHash != world.contentHash;
    Session session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    session._worldChanged = worldChanged;
    session._index.SyncHistory(session._store, session.FoldedThrough(), backend);
    return session;
}

std::optional<core::TurnRecord> Session::RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                                 std::stop_token stop)
{
    // Control characters go (newline and tab stay): they have no place in a prompt, and U+001E is the backend's
    // content marker.
    std::string kept;
    for (const char c : input) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t') {
            kept += c;
        }
    }
    const std::string cleaned = core::Trim(kept);
    if (cleaned.empty()) {
        throw TurnError("the input is empty");
    }
    return PlayTurn(cleaned, 0, callbacks, stop);
}

void Session::Undo()
{
    const int latest = _store.LatestTurn();
    if (latest == 0) {
        throw TurnError("there is no turn to undo");
    }
    _store.TruncateAfter(latest - 1);
}

std::optional<core::TurnRecord> Session::Regenerate(const TurnCallbacks& callbacks, std::stop_token stop)
{
    const int latest = _store.LatestTurn();
    if (latest == 0) {
        throw TurnError("there is no turn to regenerate");
    }
    const core::TurnRecord previous = *_store.Turn(latest);
    const nlohmann::json previousState = _store.StateAt(latest);
    _store.TruncateAfter(latest - 1);
    try {
        std::optional<core::TurnRecord> record = PlayTurn(previous.input, previous.attempt + 1, callbacks, stop);
        if (!record) {
            _store.CommitTurn(previous, previousState);
        }
        return record;
    } catch (...) {
        _store.CommitTurn(previous, previousState);
        throw;
    }
}

bool Session::Maintain(std::stop_token stop)
{
    const int latest = _store.LatestTurn();
    const nlohmann::json state = _store.StateAt(latest);
    for (;;) {
        // The next input is unknown: a placeholder tail measures the stable layers. The turn checks the real tail.
        const context::ContextInputs inputs = Inputs(state, {}, std::string(MAINTENANCE_INPUT), RESOLVE_INSTRUCTION);
        const context::Assembly assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
        if (assembly.summaryOverBudget) {
            if (!Condense(assembly.prompt.messages.front(), latest, 0, stop)) {
                return false;
            }
            continue;
        }
        if (assembly.transcriptOverBudget && !inputs.transcript.empty()) {
            if (!FoldOldest(assembly.prompt.messages.front(), inputs.transcript, latest, 0, stop)) {
                return false;
            }
            continue;
        }
        break;
    }
    _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
    return true;
}

const store::RunMeta& Session::Meta() const
{
    return _store.Meta();
}

bool Session::WorldChanged() const
{
    return _worldChanged;
}

nlohmann::json Session::State()
{
    return _store.StateAt(_store.LatestTurn());
}

std::vector<core::TurnRecord> Session::History()
{
    const int latest = _store.LatestTurn();
    return latest == 0 ? std::vector<core::TurnRecord>{} : _store.Turns(1, latest);
}

std::vector<core::Summary> Session::Summaries()
{
    return _store.Summaries();
}

const context::Assembly& Session::LastAssembly() const
{
    return _lastAssembly;
}

std::optional<core::TurnRecord> Session::PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                                  std::stop_token stop)
{
    const int turn = _store.LatestTurn() + 1;
    const nlohmann::json state = _store.StateAt(turn - 1);
    const std::uint64_t seed = _store.Meta().rngSeed;
    nlohmann::json telemetry = nlohmann::json::object();
    // Telemetry spelling of a memory kind, as the lookup call spells sources.
    auto kindText = [](store::EmbeddingKind kind) {
        switch (kind) {
        case store::EmbeddingKind::Lore:
            return "lore";
        case store::EmbeddingKind::Turn:
            return "history";
        case store::EmbeddingKind::Note:
            return "notes";
        }
        throw core::ContractError("PlayTurn: unhandled EmbeddingKind");
    };

    // 1. Retrieve.
    std::vector<RetrievalHit> hits =
        _index.Retrieve(input, ALL_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // 2. Resolve.
    std::optional<context::Assembly> assembly =
        AssembleFitting(state, hits, input, RESOLVE_INSTRUCTION, turn, attempt, stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = BuildResolveGrammar(_world, state, _mode);
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.sampling =
        inference::SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, attempt));
    const inference::Completion resolved = _pBackend->Complete(assembly->prompt, resolveSpec, {}, stop);
    if (resolved.cancelled) {
        return std::nullopt;
    }

    // 3. Apply.
    const ResolveProposal proposal = ParseResolveOutput(resolved.text);
    Resolution resolution = ApplyProposal(_world, state, proposal, seed, turn);
    core::TurnRecord record;
    record.number = turn;
    record.attempt = attempt;
    record.input = input;
    const nlohmann::json parsed = nlohmann::json::parse(resolved.text, nullptr, false);
    record.resolve = parsed.is_discarded() ? nlohmann::json{{"unparsed", resolved.text}} : parsed;
    record.checks = resolution.checks;
    record.applied = resolution.applied;
    record.rejected = resolution.rejected;
    if (_mode == core::MemoryMode::Assisted) {
        record.notes = proposal.notes;
    }
    if (callbacks.onResolved) {
        callbacks.onResolved(record);
    }

    // 4. Narrate: the resolve prompt plus the resolve output and the resolution record, so all of it stays cached.
    inference::Prompt narratePrompt = assembly->prompt;
    context::ExtendForNarrate(narratePrompt, resolved.text,
                              RenderNarrateMessage(proposal, resolution, _settings.budget.narrateAllowance, *_pBackend));
    inference::GenerationSpec narrateSpec;
    narrateSpec.maxTokens = _settings.budget.narrateMaxTokens;
    narrateSpec.sampling =
        inference::SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Narrate, attempt));
    const inference::Completion narrated =
        _pBackend->Complete(narratePrompt, narrateSpec, callbacks.onNarrationToken, stop);
    if (narrated.cancelled) {
        return std::nullopt;
    }
    record.narration = core::Trim(narrated.text);

    // 5. Commit.
    telemetry["resolve"] = Telemetry(resolved);
    telemetry["narrate"] = Telemetry(narrated);
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back({{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}});
    }
    record.timings = std::move(telemetry);
    _store.CommitTurn(record, resolution.state);
    return record;
}

context::ContextInputs Session::Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,
                                       const std::string& input, std::string_view instruction)
{
    context::ContextInputs inputs;
    inputs.engineRules = std::string(ENGINE_RULES);
    if (_mode == core::MemoryMode::Assisted) {
        inputs.engineRules += "\n\n" + std::string(ASSISTED_RULES);
    }
    inputs.summaries = _store.Summaries();
    const int latest = _store.LatestTurn();
    const int firstUnfolded = FoldedThrough() + 1;
    if (firstUnfolded <= latest) {
        inputs.transcript = _store.Turns(firstUnfolded, latest);
    }
    inputs.state = state;
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }
    inputs.playerInput = input;
    inputs.instruction = std::string(instruction);
    return inputs;
}

std::optional<context::Assembly> Session::AssembleFitting(const nlohmann::json& state, std::vector<RetrievalHit>& hits,
                                                          const std::string& input, std::string_view instruction,
                                                          int turn, int attempt, std::stop_token stop)
{
    // Every pass sheds a hit, condenses the summary below its cap, or folds turns, so the loop ends.
    for (;;) {
        const context::ContextInputs inputs = Inputs(state, hits, input, instruction);
        context::Assembly assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
        if (assembly.Fits()) {
            return assembly;
        }
        if (assembly.tailOverBudget) {
            if (hits.empty()) {
                throw TurnError(std::format("the input and the state use {} tokens, over tail_allowance {}",
                                            assembly.tokens.tail, _settings.budget.tailAllowance));
            }
            hits.pop_back();
            continue;
        }
        if (assembly.summaryOverBudget) {
            if (!Condense(assembly.prompt.messages.front(), turn, attempt, stop)) {
                return std::nullopt;
            }
            continue;
        }
        // The transcript is over its budget, or the whole prompt overflows. contextOverflow is the authoritative fit
        // check: transcriptBudget leaves out a few template tokens that it counts.
        if (!inputs.transcript.empty()) {
            if (!FoldOldest(assembly.prompt.messages.front(), inputs.transcript, turn, attempt, stop)) {
                return std::nullopt;
            }
            continue;
        }
        if (!hits.empty()) {
            hits.pop_back();
            continue;
        }
        throw TurnError("the prompt does not fit context_size even without transcript or retrieval");
    }
}

bool Session::FoldOldest(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& transcript,
                         int turn, int attempt, std::stop_token stop)
{
    const std::size_t count = std::max<std::size_t>(1, transcript.size() / 2);
    const std::vector<core::TurnRecord> oldest(transcript.begin(),
                                               transcript.begin() + static_cast<std::ptrdiff_t>(count));
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _settings.budget.summaryCap / FOLD_SUMMARY_DIVISOR);
    spec.sampling = inference::SamplingFrom(
        _settings.resolveSampling, core::SamplingSeed(_store.Meta().rngSeed, turn, core::CallKind::Summarize, attempt));
    const std::optional<core::Summary> summary = SummarizeTurns(systemMessage, oldest, spec, *_pBackend, stop);
    if (!summary) {
        return false;
    }
    _store.AddSummary(*summary);
    // Folded turns leave the transcript, so they become searchable history.
    _index.SyncHistory(_store, summary->toTurn, *_pBackend);
    return true;
}

bool Session::Condense(const inference::Message& systemMessage, int turn, int attempt, std::stop_token stop)
{
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _settings.budget.summaryCap / CONDENSE_SUMMARY_DIVISOR);
    spec.sampling = inference::SamplingFrom(
        _settings.resolveSampling, core::SamplingSeed(_store.Meta().rngSeed, turn, core::CallKind::Summarize, attempt));
    const std::optional<core::Summary> merged = CondenseSummaries(systemMessage, _store.Summaries(), spec, *_pBackend, stop);
    if (!merged) {
        return false;
    }
    _store.ReplaceSummaries(*merged);
    return true;
}

int Session::FoldedThrough()
{
    const std::vector<core::Summary> summaries = _store.Summaries();
    return summaries.empty() ? 0 : summaries.back().toTurn;
}

} // namespace morpheus::turn
```

Implementation notes:
- **`Session::Start` closes the store by moving it into a temporary.** Check `store::Database`'s move constructor, which must leave the source with a null handle, and `RunStore`'s implicit move. If `RunStore` has no move constructor, add a defaulted one in `RunStore.h` and note it in the report.
- **Nothing is ever folded if `Summaries()` is empty.** `FoldedThrough()` returns 0 then, so the whole run is the transcript.
- **The folding test's numbers assume `ENGINE_RULES` as written in Task 7.** That is about 2.2 kB, about 550 FakeBackend tokens.
  - If the test sees `DataError` "no room for the transcript", raise the `ScriptedBackend` context size in the test.
  - If it never folds, lower the context size.
  - Keep the intent: a transcript budget near two turns. Report the numbers used.
- **`AssembleFitting` has one call site in this task.** Task 11 adds the second, for the assisted-mode lookup call, so it is a private method from the start. The `kindText` lambda in `PlayTurn` serves the retrieval telemetry now and the lookup telemetry in Task 11.
- Add `Session.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session]"`
Expected: PASS.

Then run `ctest --preset tests` and `ctest --preset asan`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/turn/SessionTests.cpp
git commit -m "Add the session: retrieve, resolve, apply, narrate, commit, fold" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

