### Task 5: NarrationRestarted, the run model, replays and the terminal player

The worker turns `onNarrationRestarted` into a `NarrationRestarted` event; the app's run model clears the turn's streamed text on it, so the Play window shows only the new narration; `morpheus_play` prints `[narration rewritten: <problems>]` and streams the new text below it. Replays recognise the review grammar, replay fixtures record the run's review settings and hold `review` replies, and a turn's digest is part of what a replay checks.

**Files:**
- Modify: `src/turn/Worker.h`, `src/turn/Worker.cpp`, `src/turn/Replay.h`, `src/turn/Replay.cpp`, `src/app/model/RunModel.h`, `src/app/model/RunModel.cpp`, `tools/play/main.cpp`
- Modify: `tests/data/replays/mini-check-regenerate-note.json`, `tests/data/replays/mini-undo-regenerate.json`, `tests/data/replays/sandcrawler-playtest.json`
- Test: `tests/turn/WorkerTests.cpp`, `tests/turn/ReplayTests.cpp`, `tests/app/RunModelTests.cpp`

**Interfaces:**
- Consumes: `turn::TurnCallbacks::onNarrationRestarted` (Task 4); `turn::BuildReviewGrammar` (Task 3); `core::Settings::narrationCheck`, `compactTranscript`, `proseTurns` (Task 1); `core::TurnRecord::digest` (Task 2).
- Produces:
  - `struct turn::NarrationRestarted { std::vector<std::string> problems; }`, a non-closing `Event` alternative placed after `TokenStreamed`.
  - `turn::CallKindOf` returns `core::CallKind::Review` for a grammar starting with `BuildReviewGrammar`'s root line; renarrate calls stay `Narrate`. `turn::CallKindName(Review) == "review"`, `CallKindName(Renarrate) == "renarrate"`.
  - `turn::TurnResultJson` gains `"digest"`.
  - `nlohmann::json turn::MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode, const core::Settings& settings, const std::vector<ReplayStep>& steps, const RecordingBackend& recording, const std::vector<core::TurnRecord>& history, const nlohmann::json& finalState)`; the fixture gains `"narration_check"`, `"compact_transcript"`, `"prose_turns"`.
  - `app::RunModel::Apply(NarrationRestarted)` clears `Pending()->narration`.

- [ ] **Step 1: Write the failing worker test**

In `tests/turn/WorkerTests.cpp`, add `#include "turn/EngineText.h"` to the includes, and append:

```cpp
TEST_CASE("A renarrated turn sends NarrationRestarted between the two narrations", "[worker]")
{
    // The first review finds a contradiction, the second none; the narrations differ, so the stream shows which is which.
    int reviews = 0;
    int narrations = 0;
    test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("intent") != std::string::npos) {
            return MOVE;
        }
        if (prompt.messages.front().content == turn::REVIEW_RULES) {
            return reviews++ == 0
                       ? R"({"consistent":false,"problems":["The player flies instead of walking."],"digest":"The player flew."})"
                       : R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";
        }
        return narrations++ == 0 ? "You fly to the lighthouse." : "You walk to the lighthouse.";
    });
    core::Settings settings = TestSettings();
    settings.narrationCheck = true;
    settings.compactTranscript = true;
    settings.budget.reviewMaxTokens = 384;
    turn::Worker worker(settings, backend);
    worker.Post(Start("worker_renarrate"));
    UntilClosing(worker);

    worker.Post(turn::SubmitInput{"I walk to the lighthouse."});
    const std::vector<turn::Event> events = UntilClosing(worker);
    // Pieces before the restart are the first narration's, pieces after it the second's.
    std::string before;
    std::string after;
    int restarts = 0;
    for (const turn::Event& event : events) {
        if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&event)) {
            (restarts == 0 ? before : after) += pPiece->piece;
        } else if (const auto* pRestarted = std::get_if<turn::NarrationRestarted>(&event)) {
            ++restarts;
            CHECK(pRestarted->problems == std::vector<std::string>{"The player flies instead of walking."});
            CHECK_FALSE(turn::IsClosing(event));
        }
    }
    CHECK(restarts == 1);
    CHECK(before == "You fly to the lighthouse.");
    CHECK(after == "You walk to the lighthouse.");
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(events.back()));
    CHECK(std::get<turn::TurnCompleted>(events.back()).record.narration == "You walk to the lighthouse.");
}
```

- [ ] **Step 2: Write the failing run model test**

Append to `tests/app/RunModelTests.cpp`:

```cpp
TEST_CASE("NarrationRestarted clears the running turn's streamed narration", "[run_model]")
{
    app::RunModel model = OpenModel();
    model.OnPosted(turn::SubmitInput{"I walk."});
    model.Apply(turn::TurnResolved{Record(1, "I walk.", "")});
    model.Apply(turn::TokenStreamed{"You fly."});
    model.Apply(turn::NarrationRestarted{{"The player flies."}});
    REQUIRE(model.Pending());
    CHECK(model.Pending()->narration.empty()); // The Play window shows only the new narration.
    model.Apply(turn::TokenStreamed{"You walk."});
    CHECK(model.Pending()->narration == "You walk.");
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Turn); // It closes nothing.

    app::RunModel idle = OpenModel();
    CHECK_THROWS_AS(idle.Apply(turn::NarrationRestarted{}), core::ContractError);
}
```

- [ ] **Step 3: Write the failing replay tests**

In `tests/turn/ReplayTests.cpp`:

In `ReplaySettings()`, after the `settings.budget = {...};` line, add:

```cpp
    settings.budget.reviewMaxTokens = 384; // As settings.example.json.
```

In "Recorded sessions replay to the same turns and state", after `settings.memoryMode = *mode;`, add:

```cpp
            // The review settings the run was recorded with: they decide which review calls the replay makes.
            settings.narrationCheck = fixture.at("narration_check").get<bool>();
            settings.compactTranscript = fixture.at("compact_transcript").get<bool>();
            settings.proseTurns = fixture.at("prose_turns").get<int>();
```

Rename the test case `"CallKindOf tells the four calls apart and CallKindName spells them"` to `"CallKindOf tells the calls apart and CallKindName spells them"`, and before its closing brace add:

```cpp
    // Every review grammar starts with the same root line, whichever parts it asks for.
    for (const auto& [check, digest] : {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}}) {
        inference::GenerationSpec reviewSpec;
        reviewSpec.grammar = turn::BuildReviewGrammar(check, digest);
        CHECK(turn::CallKindOf(narration, reviewSpec) == core::CallKind::Review);
    }
    // A renarrate is a narrate call (no grammar, a narrate message): its reply joins the narrate replies.
    const inference::Prompt renarrate{{{inference::Role::System, "story"},
                                       {inference::Role::User, std::string(turn::RENARRATE_CORRECTION)}},
                                      1};
    CHECK(turn::CallKindOf(renarrate, plainSpec) == core::CallKind::Narrate);
    CHECK(turn::CallKindName(core::CallKind::Review) == "review");
    CHECK(turn::CallKindName(core::CallKind::Renarrate) == "renarrate");
```

Add `#include <utility>` to the includes.

In "RecordingBackend records each call's text by kind and MakeReplayFixture writes the format", replace the `MakeReplayFixture` call with:

```cpp
    const nlohmann::json fixture = turn::MakeReplayFixture("tests/data/worlds/mini", 9, core::MemoryMode::Engine,
                                                           ReplaySettings(), {{turn::ReplayStep::Action::Input, "I walk."}},
                                                           recording, session.History(), session.State());
```

and after `CHECK(fixture.at("memory_mode") == "engine");` add:

```cpp
    CHECK(fixture.at("narration_check") == false);
    CHECK(fixture.at("compact_transcript") == false);
    CHECK(fixture.at("prose_turns") == 2);
    CHECK(fixture.at("expected").at("turns").at(0).at("digest") == "");
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `NarrationRestarted` is not a member of `morpheus::turn`, and `MakeReplayFixture` does not take a `core::Settings`.

- [ ] **Step 5: Emit the event**

In `src/turn/Worker.h`, after `struct TokenStreamed { ... };`, add:

```cpp
// A review found that the turn's narration contradicts the outcome (`problems` are its reasons): the narration
// starts over, and the TokenStreamed events after this one are the new narration. Closes nothing.
struct NarrationRestarted {
    std::vector<std::string> problems = {};
};
```

Replace the `Event` alias with:

```cpp
using Event = std::variant<RunOpened, TokenStreamed, NarrationRestarted, TurnResolved, TurnCompleted, TurnCancelled,
                           StateChanged, MemoryUpdated, MaintenanceFailed, Error>;
```

In the events comment, replace the line

```cpp
// and UndoTurn included. MemoryUpdated and MaintenanceFailed report between-turn upkeep and close nothing.
```

with

```cpp
// and UndoTurn included. While a turn runs, TurnResolved, TokenStreamed and NarrationRestarted report its progress.
// MemoryUpdated and MaintenanceFailed report between-turn upkeep. None of these close a command.
```

In `src/turn/Worker.cpp`, replace `Worker::Callbacks` with:

```cpp
TurnCallbacks Worker::Callbacks()
{
    TurnCallbacks callbacks;
    callbacks.onResolved = [this](const core::TurnRecord& record) { Emit(TurnResolved{record}); };
    callbacks.onNarrationToken = [this](std::string_view piece) { Emit(TokenStreamed{std::string(piece)}); };
    callbacks.onNarrationRestarted = [this](const std::vector<std::string>& problems) {
        Emit(NarrationRestarted{problems});
    };
    return callbacks;
}
```

- [ ] **Step 6: Clear the pending narration in the run model**

In `src/app/model/RunModel.h`, replace the `PendingTurn` comment with:

```cpp
    // The turn being played: its input, then its resolved record (checks and changes), then the narration so far
    // (cleared when a review restarts it).
```

In `src/app/model/RunModel.cpp`, after the `TokenStreamed` branch of `Apply`, add:

```cpp
    if (std::holds_alternative<turn::NarrationRestarted>(event)) {
        // The review found that the narration contradicts the outcome: it is written again from the start.
        MORPHEUS_REQUIRE(_pending.has_value(), "RunModel: NarrationRestarted with no turn running");
        _pending->narration.clear();
        return;
    }
```

- [ ] **Step 7: Recognise the review in replays and record the review settings**

In `src/turn/Replay.h`, add `#include "core/Settings.h"` to the includes, and replace the comments and declarations of `CallKindOf`, `CallKindName`, `TurnResultJson` and `MakeReplayFixture` with:

```cpp
// Which call a completion answers, told apart the way the session makes them: a grammar that starts with the
// lookup grammar's root rule is the lookup call's, one that starts with the review grammar's root line is a review,
// any other grammar is the resolve call's, and a grammar-free call ending with the summarize or condense instruction
// is a summary; the rest is narration, renarrates included (their replies join the narrate replies, in call order).
core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec);

// Fixture spelling of a call kind: "lookup", "resolve", "narrate", "summarize", "review" or "renarrate".
std::string CallKindName(core::CallKind kind);
```

```cpp
// What a replay checks of one committed turn: {"number", "attempt", "input", "checks", "applied", "rejected",
// "notes", "narration", "digest"}, in the store's JSON forms.
nlohmann::json TurnResultJson(const core::TurnRecord& record);

// A replay fixture (format in tests/data/replays and the M3 plan): the world folder (relative to the repo root),
// seed, memory mode, the run's review settings (narration_check, compact_transcript, prose_turns), the steps
// played, every reply by kind, and what the session committed.
nlohmann::json MakeReplayFixture(const std::string& worldFolder, std::uint64_t seed, core::MemoryMode mode,
                                 const core::Settings& settings, const std::vector<ReplayStep>& steps,
                                 const RecordingBackend& recording, const std::vector<core::TurnRecord>& history,
                                 const nlohmann::json& finalState);
```

In the `RollBack` comment (the code already treats every kind but Summarize alike), replace the line

```cpp
    // Truncates the Lookup, Resolve and Narrate replies to `mark`: each kind keeps its first marked replies, and a
```

with

```cpp
    // Truncates every kind's replies except Summarize to `mark`: each kind keeps its first marked replies, and a
```

In `src/turn/Replay.cpp`, replace `CallKindOf` and `CallKindName` with:

```cpp
core::CallKind CallKindOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)
{
    // A grammar means a structured call. The lookup grammar is told by its root rule (its first line), which no
    // resolve grammar shares; searching the whole text would trip on a world with a field named "lookups". Every
    // review grammar has the same first line, whatever it asks for.
    if (!spec.grammar.empty()) {
        static const std::string LOOKUP_ROOT = [] {
            const std::string lookup = BuildLookupGrammar();
            return lookup.substr(0, lookup.find('\n'));
        }();
        static const std::string REVIEW_ROOT = [] {
            const std::string review = BuildReviewGrammar(true, true);
            return review.substr(0, review.find('\n') + 1);
        }();
        if (spec.grammar.starts_with(LOOKUP_ROOT)) {
            return core::CallKind::Lookup;
        }
        return spec.grammar.starts_with(REVIEW_ROOT) ? core::CallKind::Review : core::CallKind::Resolve;
    }
    // Without a grammar, the closing instruction tells a summary from narration (a renarrate is narration).
    if (!prompt.messages.empty()) {
        const std::string& last = prompt.messages.back().content;
        if (last == SUMMARIZE_INSTRUCTION || last == CONDENSE_INSTRUCTION) {
            return core::CallKind::Summarize;
        }
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
    case core::CallKind::Review:
        return "review";
    case core::CallKind::Renarrate:
        return "renarrate";
    }
    throw core::ContractError("CallKindName: unhandled CallKind");
}
```

In `TurnResultJson`, replace the return statement with:

```cpp
    return {{"number", record.number},   {"attempt", record.attempt}, {"input", record.input},
            {"checks", checks},          {"applied", applied},        {"rejected", rejected},
            {"notes", record.notes},     {"narration", record.narration}, {"digest", record.digest}};
```

Replace the signature of `MakeReplayFixture` with the header's, and its return statement with:

```cpp
    return {{"world", worldFolder},
            {"seed", seed},
            {"memory_mode", mode == core::MemoryMode::Engine ? "engine" : "assisted"},
            {"narration_check", settings.narrationCheck},
            {"compact_transcript", settings.compactTranscript},
            {"prose_turns", settings.proseTurns},
            {"steps", stepsJson},
            {"replies", replies},
            {"expected", {{"turns", turns}, {"final_state", finalState}}}};
```

- [ ] **Step 8: Add review replies and digests to the three fixtures by hand**

Each fixture records the review settings `narration_check` true, `compact_transcript` true, `prose_turns` 2, and gains one `review` reply per narration the run made (undone and regenerated attempts included), all consistent, with digests. The other replies stay exactly as recorded. Each committed turn's expected result gains the digest of its last review. Use the Edit tool (read each file first); every `old_string` below is unique in its file.

`tests/data/replays/mini-check-regenerate-note.json` (narrations: turn 1, its regenerate, turn 2, turn 3):

Replace `  "memory_mode": "assisted",` with:

```json
  "memory_mode": "assisted",
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
```

Replace

```json
      "\"An old shanty,\" the keeper says."
    ]
  },
```

with

```json
      "\"An old shanty,\" the keeper says."
    ],
    "review": [
      {"consistent": true, "problems": [], "digest": "The player reached for the first handhold of the cliff."},
      {"consistent": true, "problems": [], "digest": "The player climbed the cliff slowly, testing each hold."},
      {"consistent": true, "problems": [], "digest": "The player went to the lighthouse, where the keeper hummed while trimming the wick."},
      {"consistent": true, "problems": [], "digest": "The player asked about the song, and the keeper said it was an old shanty."}
    ]
  },
```

Replace `"narration": "You climb slowly, testing each hold."}` with:

```json
"narration": "You climb slowly, testing each hold.",
       "digest": "The player climbed the cliff slowly, testing each hold."}
```

Replace `"narration": "The keeper hums as he trims the wick."}` with:

```json
"narration": "The keeper hums as he trims the wick.",
       "digest": "The player went to the lighthouse, where the keeper hummed while trimming the wick."}
```

Replace `"narration": "\"An old shanty,\" the keeper says."}` with:

```json
"narration": "\"An old shanty,\" the keeper says.",
       "digest": "The player asked about the song, and the keeper said it was an old shanty."}
```

`tests/data/replays/mini-undo-regenerate.json` (narrations: turn 1, the undone turn 2, turn 2, its regenerate):

Replace `  "memory_mode": "engine",` with:

```json
  "memory_mode": "engine",
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
```

Replace

```json
      "You set the lantern in its cradle."
    ]
  },
```

with

```json
      "You set the lantern in its cradle."
    ],
    "review": [
      {"consistent": true, "problems": [], "digest": "The player walked to the lighthouse."},
      {"consistent": true, "problems": [], "digest": "The player climbed the stairs."},
      {"consistent": true, "problems": [], "digest": "The player found no oil but reached the lamp room."},
      {"consistent": true, "problems": [], "digest": "The player set the lantern in its cradle."}
    ]
  },
```

Replace `"narration": "You walk to the lighthouse."}` with:

```json
"narration": "You walk to the lighthouse.",
       "digest": "The player walked to the lighthouse."}
```

Replace `"narration": "You set the lantern in its cradle."}` with:

```json
"narration": "You set the lantern in its cradle.",
       "digest": "The player set the lantern in its cradle."}
```

`tests/data/replays/sandcrawler-playtest.json` (written by `dump(2)`; narrations: turns 1 and 2, the undone turn 3, turn 3, its regenerate):

Replace the file's first two lines

```json
{
  "expected": {
```

with

```json
{
  "compact_transcript": true,
  "expected": {
```

Replace

```json
  "memory_mode": "engine",
  "replies": {
```

with

```json
  "memory_mode": "engine",
  "narration_check": true,
  "prose_turns": 2,
  "replies": {
```

Replace

```json
    ]
  },
  "seed": 1234,
```

with

```json
    ],
    "review": [
      {"consistent": true, "problems": [], "digest": "The captain checked the fuel gauge and found the fuel steady."},
      {"consistent": true, "problems": [], "digest": "The captain scanned the horizon from the roof and saw an unfamiliar rocky ridge."},
      {"consistent": true, "problems": [], "digest": "The captain ordered the crew to ration water, and they cut back their use."},
      {"consistent": true, "problems": [], "digest": "The captain ordered half water portions, and the crew shared the water."},
      {"consistent": true, "problems": [], "digest": "The captain ordered half water portions, and the uneasy crew accepted without argument."}
    ]
  },
  "seed": 1234,
```

Replace `        "input": "I check the fuel gauge.",` (8 spaces; the `steps` copy has 6 and no comma) with:

```json
        "digest": "The captain checked the fuel gauge and found the fuel steady.",
        "input": "I check the fuel gauge.",
```

Replace `        "input": "I climb onto the roof and scan the horizon with the compass.",` with:

```json
        "digest": "The captain scanned the horizon from the roof and saw an unfamiliar rocky ridge.",
        "input": "I climb onto the roof and scan the horizon with the compass.",
```

Replace `        "input": "I order the crew to ration water, half portions for everyone.",` with:

```json
        "digest": "The captain ordered half water portions, and the uneasy crew accepted without argument.",
        "input": "I order the crew to ration water, half portions for everyone.",
```

Check that the three files still parse: `python -c "import json,sys; [json.load(open(f, encoding='utf-8')) for f in sys.argv[1:]]" tests/data/replays/*.json` prints nothing.

- [ ] **Step 9: Print the rewrite in the terminal player**

In `tools/play/main.cpp`, add to the header comment, after the line `// At the prompt: /undo, /regen, /state, /quit, or your action.`:

```cpp
// A narration the review rewrites is announced with "[narration rewritten: <problems>]", and the new one streams below.
```

In `PrintUntilClosing`, after the `TokenStreamed` branch (`std::cout << pPiece->piece << std::flush;`), add:

```cpp
        } else if (const auto* pRestarted = std::get_if<turn::NarrationRestarted>(&*event)) {
            // The review found that the narration contradicts the outcome: the rewrite streams below this line.
            std::string problems;
            for (const std::string& problem : pRestarted->problems) {
                problems += (problems.empty() ? "" : "; ") + problem;
            }
            std::cout << "\n\n[narration rewritten: " << problems << "]\n\n" << std::flush;
```

Replace the `MakeReplayFixture` call with:

```cpp
            out << turn::MakeReplayFixture(core::PathText(worldFolder), runSeed, mode, settings, steps, *recording,
                                           view.history, view.state)
                       .dump(2)
                << '\n';
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[worker],[replay],[run_model]"`
Expected: all pass. Each replay fixture now makes its review calls, uses every `review` reply, and checks every committed digest.

Then: `ctest --preset tests`
Expected: all pass.

`tools/play` builds only with the `dev` preset (llama.cpp with Vulkan). If `build/dev` is configured on this machine, run `cmake --build --preset dev --target morpheus_play` and expect a clean build; otherwise say in the task report that `morpheus_play` was not compiled.

- [ ] **Step 11: Commit**

```bash
git add src/turn/Worker.h src/turn/Worker.cpp src/turn/Replay.h src/turn/Replay.cpp src/app/model/RunModel.h src/app/model/RunModel.cpp tools/play/main.cpp tests/data/replays/mini-check-regenerate-note.json tests/data/replays/mini-undo-regenerate.json tests/data/replays/sandcrawler-playtest.json tests/turn/WorkerTests.cpp tests/turn/ReplayTests.cpp tests/app/RunModelTests.cpp
git commit -m "Report narration restarts and replay review calls" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
