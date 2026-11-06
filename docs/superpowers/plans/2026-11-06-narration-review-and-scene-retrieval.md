# Narration Review, Compact Transcript and Scene Retrieval Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After every narration a standalone review call checks the prose against the engine's outcome (a contradiction is narrated once more and replaced) and writes a digest; older transcript turns show that digest instead of their narration; automatic retrieval also searches with the current scene and the previous turn's digest.

**Architecture:**
- **Settings (`core`)**: required keys `narration_check`, `compact_transcript`, `prose_turns`, `budget.review_max_tokens`, `budget.review_thinking_tokens`. `core::CallKind` gains `Review` and `Renarrate`, appended after `Summarize`.
- **Store**: `core::TurnRecord::digest`, a `digest` column in `turns`, `SCHEMA_VERSION` 2 (version-1 runs are refused, no migration).
- **Review (`turn`)**: `BuildReviewGrammar` (in `turn/ResolveGrammar`, the module's grammar file), `Review`, `ParseReviewOutput` and `ReviewPrompt` (new `turn/Review`), `REVIEW_RULES`, `REVIEW_INSTRUCTION`, `RENARRATE_CORRECTION` (in `turn/EngineText.h`). The outcome rendering of the narrate message is extracted as `turn::RenderOutcome`, so the review prompt shows the outcome exactly as the narrate message does.
- **Session**: narrate, then review (when either switch is on), then at most one renarrate and its review, then commit. `TurnCallbacks::onNarrationRestarted` becomes the worker's `NarrationRestarted` event.
- **Compact transcript (`context`)**: `context::RenderTranscript(turns, proseTurns)`. The resolve prompt is assembled by `context::AssembleResolvePrompt`, so `ContextInputs` gains `proseTurns`, which `Session::Inputs` sets; folding always renders full narration.
- **Scene retrieval**: world.json's optional `scene` block (`core::SceneDecl`), `turn::SceneOf`, and `MemoryIndex::Retrieve` taking a `turn::RetrievalQuery` (input, scene values, digest) and returning each hit's `reasons`.
- **App and terminal player**: the Settings window edits the new keys; `RunModel` clears the pending narration on `NarrationRestarted` (the Play window draws it); the Turn log shows reviews and the replaced narration; the Memory window shows reasons and the scene; `morpheus_play` prints the rewrite line.
- **Spec:** `docs/superpowers/specs/2026-11-06-narration-review-and-scene-retrieval-design.md` (the single source of requirements) and the architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022), CMake 3.25+, nlohmann/json, SQLite, Catch2 v3, Dear ImGui (docking branch, headless in tests). No new dependencies.

**Choices this plan makes where the spec is silent (the owner may overrule any of them):**
- `BuildReviewGrammar` lives in `src/turn/ResolveGrammar.h/.cpp` (there is no `turn/Grammar` file). Every review grammar starts with the line `root ::= review`, so `CallKindOf` recognises all three combinations by that line.
- The grammar ties `problems` to the verdict: `"consistent":true` is followed by `[]`, `"consistent":false` by 1 to `REVIEW_MAX_PROBLEMS` problems. `ParseReviewOutput(text, check, digest)` applies the same rule, so an inconsistent verdict always names at least one problem.
- The review call samples with the `resolve` sampling settings (like the lookup and summary calls); the renarrate uses the `narrate` sampling settings.
- The renarrate paragraph sits inside the narrate message's `# Task` section: `RENARRATE_CORRECTION`, the problems one per line (`- ...`), a blank line, then `NARRATE_INSTRUCTION`. When the message is over `narrate_allowance` after the four existing shedding steps, a fifth step drops the problem lines (the correction sentence stays); only then is it a `DataError`. Without that step, long model-written problems could turn a model mistake into an exception.
- `RenderNarrateMessage` takes the problems as its third parameter (`{}` for the first narration); every caller is updated.
- A renarrate reply is recorded in the replay fixture's `narrate` list (`CallKindOf` returns `Narrate` for it: same prompt shape, no grammar); `CallKindName` spells `Review` "review" and `Renarrate` "renarrate".
- `TurnResultJson` gains `"digest"`, so the fixtures' `expected` turns gain their digest by hand too. `MakeReplayFixture` takes the run's `core::Settings` to record `narration_check`, `compact_transcript` and `prose_turns`.
- `core::Settings` defaults: `narrationCheck = false`, `compactTranscript = false`, `proseTurns = 2`, `reviewMaxTokens = 0`, `reviewThinkingTokens = 0`, so settings built in code (tests) behave as today. `settings.json` requires every key.
- `review_max_tokens` must be in 1..`context_size`; neither review budget joins the "room for the transcript" sum (the spec: no reservation in the context fit).
- `scene.present_at` is required. A scene whose `present_at` value is empty (after trimming) has no present entities. Deduplication compares trimmed, lower-cased values and keeps the first value as written.
- The name boost now matches the whole input; the cut to `EMBED_TEXT_CHARS` applies to the embedded text only (`RetrievalQuery::Text`).
- Every turn's telemetry gets a `"scene"` array (empty for a world without a scene); the Memory window hides an empty scene.
- `NarrationRestarted` carries the review's `problems` (for `morpheus_play`'s line) and sits after `TokenStreamed` in `turn::Event`.
- `morpheus_play`, `RunModel` and the replay changes land with the worker event (Task 5), not with the app windows (Task 9): the new event and `MakeReplayFixture`'s new parameter would otherwise leave the `dev` build or the app broken in between.
- README gains a short "Worlds" section, since it has no world-format section yet.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25. No new dependencies.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /WX /permissive- /Zc:preprocessor /utf-8`): a warning fails the build.
- **Exact names and values (from the spec, verbatim):**
  - `REVIEW_MAX_PROBLEMS = 3`, `REVIEW_PROBLEM_MAX_CHARS = 200`, `DIGEST_MAX_CHARS = 300`, `REVIEW_RETRY_OFFSET = 1 << 23`.
  - settings keys `"narration_check"`, `"compact_transcript"`, `"prose_turns"` (top level), `"review_max_tokens"`, `"review_thinking_tokens"` (in `"budget"`); example values `true`, `true`, `2`, `384`, `0`.
  - `core::CallKind::Review` and `core::CallKind::Renarrate`, appended after `Summarize`.
  - `BuildReviewGrammar(bool check, bool digest)`, `ParseReviewOutput`, `struct Review { std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`, `REVIEW_RULES`, `REVIEW_INSTRUCTION`.
  - `TurnCallbacks::onNarrationRestarted`, worker event `NarrationRestarted`.
  - `record.timings` keys `"review"` (array of one or two entries), `"renarrate"` (with `"replaced"`), each `"retrieval"` entry's `"reasons"`, top-level `"scene"`.
  - `core::TurnRecord::digest`, column `digest TEXT NOT NULL`, `SCHEMA_VERSION` 2.
  - `context::RenderTranscript(turns, proseTurns)`; the session passes `settings.proseTurns` with `compact_transcript` on and `std::numeric_limits<int>::max()` with it off; folding always passes `std::numeric_limits<int>::max()`.
  - world.json `"scene": {"paths": [...], "present_at": "..."}`, `core::World::scene` (`std::optional<SceneDecl>` with `std::vector<std::string> paths` and `std::string presentAt`).
  - `SceneOf(world, state)` in `turn/Retrieval`; `RetrievalHit::reasons` with `"name"` and `"scene"`; `NAME_BOOST` (+0.15) at most once per entry; scene values joined by `"; "`; query parts joined by newlines.
- **Error policy:** engine bugs throw `core::ContractError` via `MORPHEUS_REQUIRE`. Bad settings, world or saved data throw `core::DataError` / `store::StoreError` naming the file and key. Model mistakes (an unreadable review) are recorded with a reason in the telemetry, and the turn goes on. No fallbacks that hide a broken contract.
- **Module boundaries:** `core` has no LLM and no UI; `context` depends on `core` and `inference`; `turn` on everything below; `app/model` has no ImGui; dependencies flow downward only.
- **Coding rules (`CLAUDE.md`):** types and methods PascalCase, locals and parameters camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; constants ALL_CAPS and grouped; members initialized at declaration; class layout: nested types, public data, private data, public methods, private methods, helpers; comments let a method read top to bottom; a private helper only with more than one caller (local lambdas are fine); no legacy paths: callers move to the new contract. ASCII only in source comments and docs.
- **Tests first:** every behavior lands with a Catch2 test written and run (failing) before the implementation. Model-backed `[llama]` tests are not needed for this milestone.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash:
  - configure, build, test: `cmake --preset tests`, `cmake --build --preset tests`, `ctest --preset tests`;
  - one tag: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`.
  - New test files are picked up by the next build (`CONFIGURE_DEPENDS`).
- **Never stage** `settings.json`, `models/`, `dotnet/`, `imgui.ini`, `.superpowers/` or `docs/references/`. Stage the files each commit step names, nothing else.
- **Commit trailer:** every commit message ends with a `Co-Authored-By:` line naming the implementing model. The commit steps below show the Sonnet line, `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; an implementer running as Opus writes `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` instead.

## File Structure

```
settings.example.json                      modify: the five new keys (Task 1)
src/core/Settings.h/.cpp                   modify: new keys, validation, SettingsToJson (Task 1)
src/core/Dice.h                            modify: CallKind::Review, CallKind::Renarrate (Task 1)
src/core/TurnRecord.h                      modify: digest (Task 2)
src/store/RunStore.h/.cpp                  modify: digest column, SCHEMA_VERSION 2 (Task 2)
src/turn/ResolveGrammar.h/.cpp             modify: review limits, BuildReviewGrammar (Task 3)
src/turn/EngineText.h                      modify: REVIEW_RULES, REVIEW_INSTRUCTION, RENARRATE_CORRECTION (Task 3),
                                                   ENGINE_RULES sentence (Task 6)
src/turn/Resolve.h/.cpp                    modify: OutcomeDetail, RenderOutcome, RenderNarrateMessage(problems) (Task 3)
src/turn/Review.h/.cpp                     create: Review, ParseReviewOutput, ReviewPrompt (Task 3)
src/turn/CMakeLists.txt                    modify: Review.cpp (Task 3)
src/turn/Session.h/.cpp                    modify: review and renarrate (Task 4), proseTurns (Task 6), scene query (Task 8)
src/turn/Worker.h/.cpp                     modify: NarrationRestarted (Task 5)
src/turn/Replay.h/.cpp                     modify: CallKindOf/Name, digest, review settings in fixtures (Task 5)
src/app/model/RunModel.h/.cpp              modify: NarrationRestarted clears the pending narration (Task 5)
tools/play/main.cpp                        modify: rewrite line, MakeReplayFixture settings (Task 5)
tests/data/replays/*.json                  modify: review replies, digests, review settings (Task 5)
src/context/Transcript.h/.cpp              modify: RenderTranscript(turns, proseTurns) (Task 6)
src/context/PromptBuilder.h/.cpp           modify: ContextInputs::proseTurns (Task 6)
src/turn/Folding.cpp                       modify: full narration for folds (Task 6)
src/core/World.h/.cpp                      modify: SceneDecl, the scene block (Task 7)
game_projects/sandcrawler/world.json       modify: scene block (Task 7)
src/turn/Retrieval.h/.cpp                  modify: SceneOf, RetrievalQuery, reasons (Task 8)
tests/SceneWorld.h                         create: a mini world copy with a scene (Task 8)
src/app/model/SettingsEdit.h/.cpp          modify: the new keys (Task 9)
src/app/model/Inspect.h/.cpp               modify: ReviewRows, ReplacedNarration, SceneValues, reasons (Task 9)
src/app/ui/SettingsWindow.cpp              modify (Task 9)
src/app/ui/TurnLogWindow.cpp               modify (Task 9)
src/app/ui/MemoryWindow.cpp                modify (Task 9)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, README.md   modify (Task 10)
tests/core/SettingsTests.cpp, DiceTests.cpp, WorldTests.cpp, SandcrawlerWorldTests.cpp
tests/store/RunStoreTests.cpp
tests/turn/ResolveGrammarTests.cpp, ResolveTests.cpp, ReviewTests.cpp (create), SessionTests.cpp, WorkerTests.cpp,
           ReplayTests.cpp, RetrievalTests.cpp, FoldingTests.cpp
tests/context/RenderTests.cpp, PromptBuilderTests.cpp
tests/app/RunModelTests.cpp, SettingsEditTests.cpp, InspectTests.cpp, UiTests.cpp
```

---

### Task 1: Settings keys and call kinds

The review and the compact transcript are switched and budgeted in `settings.json`. Every new key is required, like every other key. The two new call kinds get their own sampling seeds while the existing kinds keep theirs.

**Files:**
- Modify: `src/core/Settings.h`, `src/core/Settings.cpp`, `src/core/Dice.h`, `settings.example.json`
- Modify (not staged): `D:\MorpheusEngine\settings.json`
- Test: `tests/core/SettingsTests.cpp`, `tests/core/DiceTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `core::Settings::narrationCheck` (`bool`, default `false`), `compactTranscript` (`bool`, default `false`), `proseTurns` (`int`, default `2`), declared after `memoryMode`.
  - `core::BudgetSettings::reviewMaxTokens` and `reviewThinkingTokens` (`int`, default `0`), declared after `narrateThinkingTokens`, so the positional initializers in the tests (`settings.budget = {256, ...}`) stay valid.
  - `enum class core::CallKind { Lookup, Resolve, Narrate, Summarize, Review, Renarrate }`.

- [ ] **Step 1: Write the failing settings tests**

In `tests/core/SettingsTests.cpp`, replace `ValidSettingsJson()` with:

```cpp
// A valid settings document whose model paths are relative to the settings file's folder.
nlohmann::json ValidSettingsJson()
{
    return nlohmann::json::parse(R"({
        "chat_model": "models/chat.gguf",
        "embedding_model": "models/embed.gguf",
        "embedding_query_prefix": "search_query: ",
        "embedding_document_prefix": "search_document: ",
        "context_size": 16384,
        "gpu_layers": -1,
        "narration_check": true,
        "compact_transcript": true,
        "prose_turns": 2,
        "budget": {
            "resolve_max_tokens": 512, "narrate_max_tokens": 768, "review_max_tokens": 384,
            "resolve_thinking_tokens": 0, "narrate_thinking_tokens": 0, "review_thinking_tokens": 0,
            "narrate_allowance": 512, "tail_allowance": 2048,
            "engine_rules_cap": 1024, "world_cap": 4096, "summary_cap": 2048, "retrieval_cap": 768
        },
        "sampling": {
            "resolve": {"temperature": 0.3, "top_k": 40, "top_p": 0.9},
            "narrate": {"temperature": 0.8, "top_k": 40, "top_p": 0.95}
        }
    })");
}
```

Append after the test case "LoadSettings reads the thinking budgets":

```cpp
TEST_CASE("LoadSettings reads the narration review and compact transcript settings", "[settings]")
{
    nlohmann::json json = ValidSettingsJson();
    json["narration_check"] = false;
    json["prose_turns"] = 3;
    json["budget"]["review_thinking_tokens"] = MIN_THINKING_TOKENS;
    const Settings settings = LoadSettings(WriteSettings("settings_review", json));
    CHECK_FALSE(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 3);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == MIN_THINKING_TOKENS);

    SECTION("each key is required")
    {
        for (const std::string key : {"narration_check", "compact_transcript", "prose_turns"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing.erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
        for (const std::string key : {"review_max_tokens", "review_thinking_tokens"}) {
            nlohmann::json missing = ValidSettingsJson();
            missing["budget"].erase(key);
            CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_missing_" + key, missing)),
                              ContainsSubstring(key));
        }
    }
    SECTION("prose_turns is at least 1")
    {
        json["prose_turns"] = 0;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_prose_turns_zero", json)), ContainsSubstring("prose_turns"));
    }
    SECTION("review_thinking_tokens is 0 or at least the minimum")
    {
        json["budget"]["review_thinking_tokens"] = 10;
        CHECK_THROWS_AS(LoadSettings(WriteSettings("settings_review_thinking_small", json)), DataError);
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_thinking_small", json)),
                          ContainsSubstring("review_thinking_tokens") && ContainsSubstring("at least 64"));
    }
    SECTION("review_max_tokens is positive")
    {
        json["budget"]["review_max_tokens"] = 0;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_max_zero", json)),
                          ContainsSubstring("review_max_tokens"));
    }
    SECTION("the switches are true or false")
    {
        json["narration_check"] = "yes";
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_review_switch", json)),
                          ContainsSubstring("narration_check") && ContainsSubstring("true or false"));
    }
}
```

In the test case "SettingsToJson round-trips through LoadSettings", after `json["budget"]["narrate_thinking_tokens"] = 256;` add:

```cpp
    json["narration_check"] = false;
    json["prose_turns"] = 5;
    json["budget"]["review_max_tokens"] = 200;
    json["budget"]["review_thinking_tokens"] = 64;
```

and after `CHECK(copy.memoryMode == MemoryMode::Assisted);` add:

```cpp
    CHECK(copy.narrationCheck == original.narrationCheck);
    CHECK(copy.compactTranscript == original.compactTranscript);
    CHECK(copy.proseTurns == 5);
    CHECK(copy.budget.reviewMaxTokens == 200);
    CHECK(copy.budget.reviewThinkingTokens == 64);
```

In the test case "settings.example.json loads with the current loader", replace `CHECK_NOTHROW(LoadSettings(dir / "settings.json"));` with:

```cpp
    const Settings settings = LoadSettings(dir / "settings.json");
    // The example turns the narration review and the compact transcript on.
    CHECK(settings.narrationCheck);
    CHECK(settings.compactTranscript);
    CHECK(settings.proseTurns == 2);
    CHECK(settings.budget.reviewMaxTokens == 384);
    CHECK(settings.budget.reviewThinkingTokens == 0);
```

- [ ] **Step 2: Write the failing seed test**

Append to `tests/core/DiceTests.cpp`:

```cpp
TEST_CASE("SamplingSeed gives the review and renarrate calls their own seeds and keeps the others", "[dice]")
{
    // Review and Renarrate come after Summarize, so the older kinds keep the values that saved runs and replay
    // fixtures were sampled with.
    CHECK(static_cast<int>(CallKind::Lookup) == 0);
    CHECK(static_cast<int>(CallKind::Resolve) == 1);
    CHECK(static_cast<int>(CallKind::Narrate) == 2);
    CHECK(static_cast<int>(CallKind::Summarize) == 3);
    CHECK(static_cast<int>(CallKind::Review) == 4);
    CHECK(static_cast<int>(CallKind::Renarrate) == 5);

    std::set<std::uint32_t> seeds;
    for (const CallKind kind : {CallKind::Lookup, CallKind::Resolve, CallKind::Narrate, CallKind::Summarize,
                                CallKind::Review, CallKind::Renarrate}) {
        seeds.insert(SamplingSeed(42, 3, kind, 0));
    }
    CHECK(seeds.size() == 6);

    // A turn's second review adds 2^23 to the attempt (turn::REVIEW_RETRY_OFFSET): still a valid attempt, and a
    // seed apart from the first review's and from a regenerated turn's first review.
    const std::uint32_t retry = SamplingSeed(42, 3, CallKind::Review, 1 << 23);
    CHECK(retry != SamplingSeed(42, 3, CallKind::Review, 0));
    CHECK(retry != SamplingSeed(42, 3, CallKind::Review, 1));
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `narrationCheck`, `compactTranscript`, `proseTurns`, `reviewMaxTokens` and `reviewThinkingTokens` are not members, and `Review` / `Renarrate` are not members of `morpheus::core::CallKind`.

- [ ] **Step 4: Add the call kinds**

In `src/core/Dice.h`, replace the `CallKind` declaration and its comment with:

```cpp
// Which model call a sampling seed is for. New kinds are appended, so the existing ones keep their values (and every
// saved run and replay fixture its seeds).
enum class CallKind { Lookup, Resolve, Narrate, Summarize, Review, Renarrate };
```

- [ ] **Step 5: Add the settings fields**

In `src/core/Settings.h`, replace the last two fields of `BudgetSettings` (the comment line above them stays) with:

```cpp
    int resolveThinkingTokens = 0; // The resolve call's thinking block; 0 turns its thinking off.
    int narrateThinkingTokens = 0; // The narrate call's thinking block; 0 turns its thinking off.
    // The review call: its prompt is standalone and short, so neither is reserved in the resolve prompt's context.
    int reviewMaxTokens = 0;      // The review call's answer (its JSON).
    int reviewThinkingTokens = 0; // The review call's thinking block; 0 turns its thinking off.
```

Replace the `Settings` struct with:

```cpp
// settings.json: per-machine configuration. Not committed; settings.example.json documents it.
struct Settings {
    std::filesystem::path chatModel = {};      // Relative paths are resolved against the settings file's folder.
    std::filesystem::path embeddingModel = {};
    std::string embeddingQueryPrefix = {};     // Prepended to retrieval queries (model-specific).
    std::string embeddingDocumentPrefix = {};  // Prepended to embedded documents (model-specific).
    int contextSize = 0;
    int gpuLayers = 0;                         // -1 puts every layer on the GPU.
    BudgetSettings budget = {};
    SamplingSettings resolveSampling = {};
    SamplingSettings narrateSampling = {};
    std::optional<MemoryMode> memoryMode = {}; // Overrides the world's default memory_mode when set.
    // A review call after each narration checks it against the outcome; a contradiction is narrated once more.
    bool narrationCheck = false;
    // The review also writes the turn's digest, and older transcript turns show it instead of their narration.
    bool compactTranscript = false;
    int proseTurns = 2; // With compactTranscript: the newest turns that keep their narration in the transcript (>= 1).
};
```

Replace the `LoadSettings` comment with:

```cpp
// Loads and validates settings.json. Throws DataError naming the file and key at fault. Unknown keys are errors,
// both model files must exist, and the budget must leave room for the transcript (L3) and SETTINGS_TEMPLATE_MARGIN
// chat-template tokens; the review budgets are not part of that sum. memory_mode is optional; every other key is
// required.
```

- [ ] **Step 6: Load, validate and write the new keys**

In `src/core/Settings.cpp`:

Add `#include <limits>` to the includes.

After the `requireString` lambda, add:

```cpp
    auto requireBool = [&](const nlohmann::json& object, const char* key, const char* context) {
        const nlohmann::json& value = requireMember(object, key, context);
        if (!value.is_boolean()) {
            throw DataError(std::format("{}: {} '{}' must be true or false", where, context, key));
        }
        return value.get<bool>();
    };
```

Replace the top-level `checkKeys` call with:

```cpp
    checkKeys(json, "settings", {"chat_model", "embedding_model", "embedding_query_prefix", "embedding_document_prefix",
                                 "context_size", "gpu_layers", "budget", "sampling", "memory_mode", "narration_check",
                                 "compact_transcript", "prose_turns"});
```

After the `memory_mode` block (before `// Budget: every part positive and below the context size.`), add:

```cpp
    // The narration review, and the compact transcript that shows older turns as the review's digest.
    settings.narrationCheck = requireBool(json, "narration_check", "settings");
    settings.compactTranscript = requireBool(json, "compact_transcript", "settings");
    settings.proseTurns = requireInt(json, "prose_turns", "settings", 1, std::numeric_limits<int>::max());
```

Replace the budget `checkKeys` call with:

```cpp
    checkKeys(budget, "budget", {"resolve_max_tokens", "narrate_max_tokens", "review_max_tokens",
                                 "resolve_thinking_tokens", "narrate_thinking_tokens", "review_thinking_tokens",
                                 "narrate_allowance", "tail_allowance", "engine_rules_cap", "world_cap", "summary_cap",
                                 "retrieval_cap"});
```

After `settings.budget.narrateMaxTokens = requireInt(budget, "narrate_max_tokens", "budget", 1, contextSize);` add:

```cpp
    settings.budget.reviewMaxTokens = requireInt(budget, "review_max_tokens", "budget", 1, contextSize);
```

After `settings.budget.narrateThinkingTokens = requireThinking("narrate_thinking_tokens");` add:

```cpp
    settings.budget.reviewThinkingTokens = requireThinking("review_thinking_tokens");
```

Above `const long long fixed = ...`, add the comment line:

```cpp
    // The review call is standalone (its own short prompt), so its budgets are not reserved here.
```

In `SettingsToJson`, replace the `json` initializer's entries from `{"gpu_layers", settings.gpuLayers},` through the end of the `"budget"` entry with:

```cpp
        {"gpu_layers", settings.gpuLayers},
        {"narration_check", settings.narrationCheck},
        {"compact_transcript", settings.compactTranscript},
        {"prose_turns", settings.proseTurns},
        {"budget",
         {{"resolve_max_tokens", settings.budget.resolveMaxTokens},
          {"narrate_max_tokens", settings.budget.narrateMaxTokens},
          {"review_max_tokens", settings.budget.reviewMaxTokens},
          {"resolve_thinking_tokens", settings.budget.resolveThinkingTokens},
          {"narrate_thinking_tokens", settings.budget.narrateThinkingTokens},
          {"review_thinking_tokens", settings.budget.reviewThinkingTokens},
          {"narrate_allowance", settings.budget.narrateAllowance},
          {"tail_allowance", settings.budget.tailAllowance},
          {"engine_rules_cap", settings.budget.engineRulesCap},
          {"world_cap", settings.budget.worldCap},
          {"summary_cap", settings.budget.summaryCap},
          {"retrieval_cap", settings.budget.retrievalCap}}},
```

- [ ] **Step 7: Add the keys to settings.example.json**

In `settings.example.json`, replace

```json
  "gpu_layers": 24,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "resolve_thinking_tokens": 2048,
    "narrate_thinking_tokens": 1536,
```

with

```json
  "gpu_layers": 24,
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
  "budget": {
    "resolve_max_tokens": 512,
    "narrate_max_tokens": 768,
    "review_max_tokens": 384,
    "resolve_thinking_tokens": 2048,
    "narrate_thinking_tokens": 1536,
    "review_thinking_tokens": 0,
```

- [ ] **Step 8: Add the keys to this machine's settings.json (never staged)**

`D:\MorpheusEngine\settings.json` is gitignored and per machine; the app and `morpheus_play` refuse it without the new keys. Add, keeping every value it already has: after its `"gpu_layers"` line

```json
  "narration_check": true,
  "compact_transcript": true,
  "prose_turns": 2,
```

and in its `"budget"` object, after `"narrate_max_tokens"`, `"review_max_tokens": 384,` and after `"narrate_thinking_tokens"`, `"review_thinking_tokens": 0,`. Do not stage it.

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings],[dice],[settings_edit]"`
Expected: all pass.

Then run the whole suite: `ctest --preset tests`
Expected: all pass. (The app tests copy `settings.example.json`; nothing reads the new keys yet.)

- [ ] **Step 10: Commit**

```bash
git add src/core/Settings.h src/core/Settings.cpp src/core/Dice.h settings.example.json tests/core/SettingsTests.cpp tests/core/DiceTests.cpp
git commit -m "Add the narration review and compact transcript settings" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The turn's digest in the record and the store

A turn keeps the last review's digest. The `turns` table gains its column, which bumps `SCHEMA_VERSION` to 2; that also covers the `turns.attempt` column M3 added without a bump. A version-1 run is refused with the existing error; there is no migration.

**Files:**
- Modify: `src/core/TurnRecord.h`, `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Test: `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `std::string core::TurnRecord::digest` (default `{}`, after `narration`); `store::RunStore::SCHEMA_VERSION == 2`; `CommitTurn` stores and `Turn` / `Turns` read the digest.

- [ ] **Step 1: Write the failing tests**

Append to `tests/store/RunStoreTests.cpp`:

```cpp
TEST_CASE("A turn's digest round-trips", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_digest"), TestMeta(), TestInitialState());
    morpheus::core::TurnRecord record;
    record.number = 1;
    record.input = "I wait.";
    record.narration = "Time passes.";
    record.digest = "The player waited.";
    store.CommitTurn(record, TestInitialState());
    CHECK(store.Turn(1)->digest == "The player waited.");
    CHECK(store.Turns(1, 1).at(0).digest == "The player waited.");

    // A turn without a review stores an empty digest.
    morpheus::core::TurnRecord quiet;
    quiet.number = 2;
    quiet.input = "I wait again.";
    store.CommitTurn(quiet, TestInitialState());
    CHECK(store.Turn(2)->digest.empty());
}

TEST_CASE("RunStore refuses a version-1 run, naming the file and both versions", "[store]")
{
    CHECK(RunStore::SCHEMA_VERSION == 2);
    const std::filesystem::path file = FreshDbPath("store_version_1");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '1' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_AS(RunStore::Open(file), StoreError);
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version 1 is not the supported version 2"));
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `digest` is not a member of `morpheus::core::TurnRecord`.

- [ ] **Step 3: Add the digest to the record**

In `src/core/TurnRecord.h`, after `std::string narration = {};` add:

```cpp
    std::string digest = {}; // The last review's one or two sentence digest; empty when there was none.
```

- [ ] **Step 4: Store it**

In `src/store/RunStore.h`, replace `static constexpr int SCHEMA_VERSION = 1;` with:

```cpp
    // 2: turns.digest. It also covers turns.attempt, which M3 added without a bump. Older runs are refused.
    static constexpr int SCHEMA_VERSION = 2;
```

and replace the comment above `static RunStore Open(...)` with:

```cpp
    // Opens an existing run. Throws StoreError if it is missing or was written with another schema version (there
    // is no migration), or its meta (rng_seed, settings) is damaged.
```

In `src/store/RunStore.cpp`, replace the `turns` table in `SCHEMA_SQL` with:

```sql
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    attempt   INTEGER NOT NULL,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    checks    TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    digest    TEXT NOT NULL,
    timings   TEXT NOT NULL
);
```

In `CommitTurn`, replace the turn insert with:

```cpp
        _db.Prepare("INSERT INTO turns (number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                    "timings) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)")
            .Bind(1, record.number)
            .Bind(2, record.attempt)
            .Bind(3, record.input)
            .Bind(4, record.resolve.dump())
            .Bind(5, checks.dump())
            .Bind(6, applied.dump())
            .Bind(7, rejected.dump())
            .Bind(8, record.narration)
            .Bind(9, record.digest)
            .Bind(10, record.timings.dump())
            .Run();
```

In `Turns`, replace the query with:

```cpp
    Statement query = _db.Prepare("SELECT number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                                  "timings FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
```

and replace

```cpp
        record.narration = query.ColumnText(7);
        record.timings = parseColumn(8, "timings");
```

with

```cpp
        record.narration = query.ColumnText(7);
        record.digest = query.ColumnText(8);
        record.timings = parseColumn(9, "timings");
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/core/TurnRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/RunStoreTests.cpp
git commit -m "Store each turn's digest and move the run schema to version 2" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
### Task 3: The review grammar, its parser, its prompt and the engine text

The review call is standalone. Its system message is `REVIEW_RULES`; its user message holds the player's input, the outcome rendered exactly as the narrate message renders it, the narration, then `REVIEW_INSTRUCTION`. A grammar fixes the output's shape for each combination of the narration check and the digest. This task also extracts the outcome rendering from `RenderNarrateMessage` as `RenderOutcome`, and gives `RenderNarrateMessage` the renarrate's correction paragraph. Nothing calls the review yet.

**Files:**
- Create: `src/turn/Review.h`, `src/turn/Review.cpp`, `tests/turn/ReviewTests.cpp`
- Modify: `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/turn/EngineText.h`, `src/turn/Resolve.h`, `src/turn/Resolve.cpp`, `src/turn/Session.cpp` (the one `RenderNarrateMessage` call), `src/turn/CMakeLists.txt`
- Test: `tests/turn/ResolveGrammarTests.cpp`, `tests/turn/ResolveTests.cpp`, `tests/turn/ReviewTests.cpp`

**Interfaces:**
- Consumes: `turn::ResolveProposal`, `turn::Resolution`, `context::RenderChangePart`, `core::Trim`.
- Produces:
  - `constexpr int turn::REVIEW_MAX_PROBLEMS = 3`, `REVIEW_PROBLEM_MAX_CHARS = 200`, `DIGEST_MAX_CHARS = 300` (`turn/ResolveGrammar.h`).
  - `std::string turn::BuildReviewGrammar(bool check, bool digest)`; its first line is always `root ::= review`.
  - `turn::REVIEW_RULES`, `turn::REVIEW_INSTRUCTION`, `turn::RENARRATE_CORRECTION` (`std::string_view`, `turn/EngineText.h`).
  - `struct turn::OutcomeDetail { bool rejected = true; std::size_t appliedShown = SIZE_MAX; bool stakes = true; bool cutIntent = false; }` and `std::string turn::RenderOutcome(const ResolveProposal&, const Resolution&, const OutcomeDetail&)` (`turn/Resolve.h`).
  - `std::string turn::RenderNarrateMessage(const ResolveProposal&, const Resolution&, const std::vector<std::string>& problems, int allowance, inference::LlmBackend&)` (the problems are new, third).
  - `struct turn::Review { std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`, `turn::Review turn::ParseReviewOutput(std::string_view text, bool check, bool digest)`, `inference::Prompt turn::ReviewPrompt(const std::string& input, const ResolveProposal&, const Resolution&, const std::string& narration)` (`turn/Review.h`).

- [ ] **Step 1: Write the failing grammar test**

Append to `tests/turn/ResolveGrammarTests.cpp`:

```cpp
TEST_CASE("The review grammar asks for the verdict, the digest or both", "[grammar]")
{
    // Both: consistent with no problems, or inconsistent with one to three of them; the digest follows the verdict.
    const std::string both = turn::BuildReviewGrammar(true, true);
    CHECK(both.starts_with("root ::= review\n"));
    CHECK_THAT(both, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" ",\"digest\":" digest-text "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}")"));
    CHECK_THAT(both, ContainsSubstring(R"(problem-text ::= "\"" char{0,200} "\"")"));
    CHECK_THAT(both, ContainsSubstring(R"(digest-text ::= "\"" char{0,300} "\"")"));
    CHECK(GrammarProblem(both).empty());

    // The check alone: no digest.
    const std::string check = turn::BuildReviewGrammar(true, false);
    CHECK(check.starts_with("root ::= review\n"));
    CHECK_THAT(check, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" "}")"));
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
```

- [ ] **Step 2: Write the failing narrate message tests**

`RenderNarrateMessage` takes the problems as its third argument; update every existing call in `tests/turn/ResolveTests.cpp` (two of them span two lines):

```bash
sed -i -E 's/RenderNarrateMessage\((proposal|unreadable), (resolution|nothing), /RenderNarrateMessage(\1, \2, {}, /' tests/turn/ResolveTests.cpp
sed -i 's/, 42, 1), 4096, backend));/, 42, 1), {}, 4096, backend));/' tests/turn/ResolveTests.cpp
grep -n -A1 "RenderNarrateMessage(" tests/turn/ResolveTests.cpp
```

The grep must show five arguments, the third `{}`, in every call (14 calls before the new tests).

Append to `tests/turn/ResolveTests.cpp`:

```cpp
TEST_CASE("RenderOutcome is the record the narrate message starts with", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const std::string outcome = turn::RenderOutcome(proposal, resolution, {});
    CHECK(outcome.starts_with("# Resolution\nIntent: climb\nCheck: agility vs hard (16): rolled "));
    CHECK_THAT(outcome, ContainsSubstring(". Stakes: a fall\n"));
    CHECK(outcome.ends_with("\n"));
    CHECK(turn::RenderNarrateMessage(proposal, resolution, {}, 512, backend) ==
          outcome + "\n# Task\n" + std::string(turn::NARRATE_INSTRUCTION));

    // Less detail: no stakes, no applied entries (two were applied), the intent cut.
    const turn::OutcomeDetail least = {.rejected = false, .appliedShown = 0, .stakes = false, .cutIntent = true};
    const std::string shed = turn::RenderOutcome(proposal, resolution, least);
    CHECK_THAT(shed, !ContainsSubstring("Stakes:"));
    CHECK_THAT(shed, ContainsSubstring("Applied: and 2 more\n"));
}

TEST_CASE("A renarrate's narrate message carries the review's problems, shed last", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const std::vector<std::string> problems = {"The narration has the player fly.", std::string(200, 'p')};

    // The correction and the problems sit between the record and the instruction.
    const std::string message = turn::RenderNarrateMessage(proposal, resolution, problems, 4096, backend);
    CHECK(message == turn::RenderOutcome(proposal, resolution, {}) + "\n# Task\n" +
                         std::string(turn::RENARRATE_CORRECTION) + "\n- The narration has the player fly.\n- " +
                         std::string(200, 'p') + "\n\n" + std::string(turn::NARRATE_INSTRUCTION));

    // Over the allowance, the record's detail goes first and the problem lines last; the correction stays.
    const turn::OutcomeDetail least = {.rejected = false, .appliedShown = 0, .stakes = false, .cutIntent = true};
    const std::string minimal = turn::RenderOutcome(proposal, resolution, least) + "\n# Task\n" +
                                std::string(turn::RENARRATE_CORRECTION) + "\n\n" +
                                std::string(turn::NARRATE_INSTRUCTION);
    const int allowance = backend.CountTokens(minimal);
    CHECK(turn::RenderNarrateMessage(proposal, resolution, problems, allowance, backend) == minimal);
    CHECK_THROWS_AS(turn::RenderNarrateMessage(proposal, resolution, problems, allowance - 1, backend),
                    core::DataError);
}
```

- [ ] **Step 3: Write the failing review tests**

Create `tests/turn/ReviewTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"
#include "turn/Review.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("ParseReviewOutput reads a verdict, its problems and a digest", "[review]")
{
    const turn::Review consistent =
        turn::ParseReviewOutput(R"({"consistent":true,"problems":[],"digest":"  The player waited. "})", true, true);
    CHECK(consistent.error.empty());
    CHECK(consistent.consistent == true);
    CHECK(consistent.problems.empty());
    CHECK(consistent.digest == "The player waited."); // Trimmed.

    const turn::Review contradicts =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["The roll failed."]})", true, false);
    CHECK(contradicts.error.empty());
    CHECK(contradicts.consistent == false);
    CHECK(contradicts.problems == std::vector<std::string>{"The roll failed."});
    CHECK(contradicts.digest.empty());

    // With the check off only the digest is read: a verdict in the output anyway is ignored.
    const turn::Review digestOnly =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["x"],"digest":"The player waited."})", false, true);
    CHECK(digestOnly.error.empty());
    CHECK_FALSE(digestOnly.consistent);
    CHECK(digestOnly.problems.empty());
    CHECK(digestOnly.digest == "The player waited.");
}

TEST_CASE("ParseReviewOutput reports output it cannot read and keeps nothing of it", "[review]")
{
    // Each case: the output, the parts asked for, and a piece of the reason.
    struct Case {
        const char* text = "";
        bool check = false;
        bool digest = false;
        const char* reason = "";
    };
    const std::vector<Case> cases = {
        {R"({"consistent":false,"probl)", true, true, "not complete JSON"}, // Cut off by review_max_tokens.
        {R"(["consistent"])", true, true, "not a JSON object"},
        {R"({"problems":[],"digest":"x"})", true, true, "'consistent'"},
        {R"({"consistent":true,"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[1],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":false,"problems":[],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":["odd"],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[]})", true, true, "'digest'"},
        {R"({"digest":7})", false, true, "'digest'"},
    };
    for (const Case& entry : cases) {
        INFO(entry.text);
        const turn::Review review = turn::ParseReviewOutput(entry.text, entry.check, entry.digest);
        CHECK_THAT(review.error, ContainsSubstring(entry.reason));
        CHECK_FALSE(review.consistent);
        CHECK(review.problems.empty());
        CHECK(review.digest.empty());
    }
    CHECK_THROWS_AS(turn::ParseReviewOutput("{}", false, false), core::ContractError);
}

TEST_CASE("ReviewPrompt shows the input, the whole outcome and the narration under the review rules", "[review]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks"},)"
        R"({"op":"add","path":"player.inventory.oil","value":-1,"reason":"spills"}]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const inference::Prompt prompt =
        turn::ReviewPrompt("I walk to the lighthouse.", proposal, resolution, "You walk to the lighthouse.");
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.stableCount == 1);
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == turn::REVIEW_RULES);
    CHECK(prompt.messages[1].role == inference::Role::User);
    CHECK(prompt.messages[1].content == "# Player\nI walk to the lighthouse.\n\n" +
                                            turn::RenderOutcome(proposal, resolution, {}) +
                                            "\n# Narration\nYou walk to the lighthouse.\n\n# Task\n" +
                                            std::string(turn::REVIEW_INSTRUCTION));
    // The outcome is shown in full, rejected changes and their reasons included.
    CHECK_THAT(prompt.messages[1].content,
               ContainsSubstring("Rejected: add player.inventory.oil (no item 'oil' to decrease)"));
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `BuildReviewGrammar`, `RenderOutcome`, `OutcomeDetail`, `RENARRATE_CORRECTION`, `ParseReviewOutput`, `ReviewPrompt`, `REVIEW_RULES` and `REVIEW_INSTRUCTION` are not members of `morpheus::turn`, `turn/Review.h` does not exist, and `RenderNarrateMessage` does not take five arguments.

- [ ] **Step 5: Add the engine text**

In `src/turn/EngineText.h`, after `NARRATE_INSTRUCTION`, add:

```cpp
// Opens a renarrate's correction, which the narrate message gains before NARRATE_INSTRUCTION; the review's problems
// follow it, one "- " line each (RenderNarrateMessage).
inline constexpr std::string_view RENARRATE_CORRECTION =
    "Your previous narration of this outcome contradicted the resolution above. Narrate the outcome exactly as the "
    "resolution states it.";

// The review call's system message: the reviewer's job, what counts as a contradiction, and what a digest is. Which
// fields the output holds is fixed by the call's grammar (BuildReviewGrammar).
inline constexpr std::string_view REVIEW_RULES = R"(# Review
You review one turn of a solo text role-playing game. A game engine decided the turn's outcome: it rolled the checks, and it applied or rejected the changes the player's action proposed. A narrator then told the player what happened. Reply with one JSON object and nothing else. It holds some of these fields:
- "consistent": false when the narration contradicts the outcome, true otherwise. A contradiction is a failed check narrated as a success or a successful one narrated as a failure, an applied change left out or reversed, or a rejected change narrated as if it happened. Tone, style and details the outcome does not decide are not contradictions.
- "problems": each contradiction in one short sentence, at most three. Empty when the narration is consistent.
- "digest": the turn in one or two sentences, past tense, third person: who did what and what came of it. No dice, roll numbers or engine terms.)";

// Ends the review call's user message, after the player's input, the outcome and the narration.
inline constexpr std::string_view REVIEW_INSTRUCTION =
    "Review the narration above against the resolution. Reply with the JSON object only.";
```

- [ ] **Step 6: Add the review grammar**

In `src/turn/ResolveGrammar.h`, after `constexpr int MAX_ITEM_CHARS     = 32;  // New counter items; held items are matched by name.`, add:

```cpp
constexpr int REVIEW_MAX_PROBLEMS      = 3;   // Contradictions one review may list.
constexpr int REVIEW_PROBLEM_MAX_CHARS = 200; // Each of them.
constexpr int DIGEST_MAX_CHARS         = 300; // A turn's digest.
```

Replace the `static_assert` with:

```cpp
// Every list bound is written as "first element, then up to bound - 1 more", so each needs at least 2.
static_assert(MAX_CHECKS >= 2 && MAX_CHANGES >= 2 && MAX_BRANCH_CHANGES >= 2 && MAX_NOTES >= 2 && MAX_LOOKUPS >= 2 &&
                  MAX_LIST_ITEMS >= 2 && REVIEW_MAX_PROBLEMS >= 2,
              "ResolveGrammar: every list bound must be at least 2");
```

After the `BuildLookupGrammar` declaration, add:

```cpp
// GBNF for the review call (architecture spec, "Turn flow"). Its first line is always "root ::= review". The output
// is one compact JSON object:
//   check:  {"consistent":true,"problems":[]} or {"consistent":false,"problems":[1 to REVIEW_MAX_PROBLEMS strings of
//           at most REVIEW_PROBLEM_MAX_CHARS]}
//   digest: "digest": a string of at most DIGEST_MAX_CHARS, after the verdict, or alone: {"digest": str}
// Throws ContractError unless `check` or `digest` is set.
std::string BuildReviewGrammar(bool check, bool digest);
```

In `src/turn/ResolveGrammar.cpp`, after `BuildLookupGrammar`, add:

```cpp
std::string BuildReviewGrammar(bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "BuildReviewGrammar: needs the check, the digest or both");
    // The digest closes the object when it is asked for: after the verdict, or alone.
    const std::string digestPart = digest ? Literal(check ? ",\"digest\":" : "{\"digest\":") + " digest-text " : "";
    std::string review;
    if (check) {
        // Consistent with no problems, or inconsistent with 1 to REVIEW_MAX_PROBLEMS of them.
        review = Literal("{\"consistent\":true,\"problems\":[]") + " " + digestPart + Literal("}") + " | " +
                 Literal("{\"consistent\":false,\"problems\":[") +
                 std::format(" problem-text ({} problem-text){{0,{}}} ", Literal(","), REVIEW_MAX_PROBLEMS - 1) +
                 Literal("]") + " " + digestPart + Literal("}");
    } else {
        review = digestPart + Literal("}");
    }

    // A fixed root line, whatever is asked for, so the call is recognised by it (CallKindOf).
    std::string grammar = "root ::= review\nreview ::= " + review + "\n";
    if (check) {
        grammar += std::format(R"(problem-text ::= "\"" char{{0,{}}} "\"")", REVIEW_PROBLEM_MAX_CHARS) + "\n";
    }
    if (digest) {
        grammar += std::format(R"(digest-text ::= "\"" char{{0,{}}} "\"")", DIGEST_MAX_CHARS) + "\n";
    }
    return grammar + Terminals();
}
```

- [ ] **Step 7: Extract RenderOutcome and add the correction**

In `src/turn/Resolve.h`, add `#include <limits>` to the includes, and replace the `RenderNarrateMessage` comment and declaration with:

```cpp
// How much of the outcome RenderOutcome shows. The defaults show everything; RenderNarrateMessage sheds detail
// from them to fit the narrate allowance.
struct OutcomeDetail {
    bool rejected = true;                                               // The Rejected line.
    std::size_t appliedShown = std::numeric_limits<std::size_t>::max(); // Applied entries shown; the rest are counted.
    bool stakes = true;                                                 // Each check line's ". Stakes: ..." part.
    bool cutIntent = false; // The intent cut to INTENT_SHORT_CHARS bytes and ended with "...".
};

// The resolution record, as the narrate message and the review prompt show it:
//   # Resolution
//   Intent: climb
//   Check: agility vs hard (16): rolled 11 +3 = 14, failure. Stakes: a fall
//   Applied: player.health -2
//   Rejected: add player.inventory.oil (no item 'oil' to decrease)
// "Applied: none" when nothing applied, "Applied: a; b; and 2 more" when entries are left out, and no Rejected line
// when nothing was rejected. An unreadable proposal gets "The action could not be resolved (<error>). Nothing
// changed." instead. Ends with a newline.
std::string RenderOutcome(const ResolveProposal& proposal, const Resolution& resolution, const OutcomeDetail& detail);

// The user message the narrate call appends: RenderOutcome, then "\n# Task\n", then for a renarrate (non-empty
// `problems`, the review's) RENARRATE_CORRECTION and one "- <problem>" line per problem and a blank line, then
// NARRATE_INSTRUCTION. When the text exceeds `allowance` tokens (budget.narrate_allowance), detail is shed one step
// at a time, only while still over:
//   1. the Rejected line;
//   2. Applied entries from the end ("and N more");
//   3. every check line's ". Stakes: ..." part;
//   4. the intent, cut to INTENT_SHORT_CHARS bytes and ended with "...";
//   5. a renarrate's problem lines (the correction sentence stays).
// Throws DataError if it still does not fit (narrate_allowance is too small). An unreadable proposal's message has
// nothing to shed and is returned as it is.
std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution,
                                 const std::vector<std::string>& problems, int allowance,
                                 inference::LlmBackend& backend);
```

In `src/turn/Resolve.cpp`, replace the whole `RenderNarrateMessage` function with:

```cpp
std::string RenderOutcome(const ResolveProposal& proposal, const Resolution& resolution, const OutcomeDetail& detail)
{
    if (!proposal.error.empty()) {
        return std::format("# Resolution\nThe action could not be resolved ({}). Nothing changed.\n", proposal.error);
    }
    // Model-written parts in a "; "-separated list: the first `count` of them.
    auto join = [](const std::vector<std::string>& parts, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (i == 0 ? "" : "; ") + parts[i];
        }
        return text;
    };

    // The intent, cut short when asked.
    std::string intent = proposal.intent;
    if (detail.cutIntent) {
        intent = core::CutUtf8(intent, INTENT_SHORT_CHARS) + (intent.size() > INTENT_SHORT_CHARS ? "..." : "");
    }
    std::string text = "# Resolution\nIntent: " + intent + "\n";

    // One line per rolled check; the stakes are model-written text and can be left out.
    for (const core::CheckOutcome& check : resolution.checks) {
        std::string rolls;
        for (const int roll : check.result.rolls) {
            rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
        }
        // A stat of 0 adds nothing, so its "+N" is left out.
        const std::string stat = check.result.statValue != 0 ? std::format(" {:+}", check.result.statValue) : "";
        text += std::format("Check: {} vs {} ({}): rolled {}{} = {}, {}", check.stat, check.difficulty,
                            check.result.target, rolls, stat, check.result.total,
                            check.result.success ? "success" : "failure");
        text += detail.stakes ? std::format(". Stakes: {}\n", check.stakes) : "\n";
    }

    // The applied changes: the first appliedShown of them, and how many more there were.
    std::vector<std::string> applied;
    for (const core::AppliedChange& change : resolution.applied) {
        applied.push_back(context::RenderChangePart(change));
    }
    if (applied.empty()) {
        text += "Applied: none\n";
    } else {
        const std::size_t shown = std::min(detail.appliedShown, applied.size());
        text += "Applied: " + join(applied, shown);
        if (shown < applied.size()) {
            text += std::format("{}and {} more", shown == 0 ? "" : "; ", applied.size() - shown);
        }
        text += "\n";
    }

    // The rejected changes with their reasons, named by op and path when the raw change has them.
    if (detail.rejected && !resolution.rejected.empty()) {
        std::vector<std::string> rejected;
        for (const core::RejectedChange& change : resolution.rejected) {
            const nlohmann::json& raw = change.raw;
            const bool named = raw.is_object() && raw.contains("op") && raw.at("op").is_string() &&
                               raw.contains("path") && raw.at("path").is_string();
            rejected.push_back(named ? std::format("{} {} ({})", raw.at("op").get<std::string>(),
                                                   raw.at("path").get<std::string>(), change.reason)
                                     : change.reason);
        }
        text += "Rejected: " + join(rejected, rejected.size()) + "\n";
    }
    return text;
}

std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution,
                                 const std::vector<std::string>& problems, int allowance,
                                 inference::LlmBackend& backend)
{
    // The task: for a renarrate the correction and, unless shed, the review's problems; then the instruction.
    auto task = [&problems](bool showProblems) {
        std::string text = "\n# Task\n";
        if (!problems.empty()) {
            text += std::string(RENARRATE_CORRECTION) + "\n";
            if (showProblems) {
                for (const std::string& problem : problems) {
                    text += "- " + problem + "\n";
                }
            }
            text += "\n";
        }
        return text + std::string(NARRATE_INSTRUCTION);
    };
    if (!proposal.error.empty()) {
        // Nothing was resolved, so the record holds nothing to shed.
        return RenderOutcome(proposal, resolution, {}) + task(true);
    }

    // Render, then shed detail one step at a time until the message fits the narrate allowance: the rejected
    // changes, applied changes from the end, the stakes, the intent's tail, then a renarrate's problems. Each step
    // runs only while over.
    OutcomeDetail detail;
    detail.rejected = !resolution.rejected.empty();
    detail.appliedShown = resolution.applied.size();
    bool showProblems = !problems.empty();
    for (;;) {
        const std::string text = RenderOutcome(proposal, resolution, detail) + task(showProblems);
        if (backend.CountTokens(text) <= allowance) {
            return text;
        }
        if (detail.rejected) {
            detail.rejected = false;
        } else if (detail.appliedShown > 0) {
            --detail.appliedShown;
        } else if (detail.stakes) {
            detail.stakes = false;
        } else if (!detail.cutIntent) {
            detail.cutIntent = true;
        } else if (showProblems) {
            showProblems = false;
        } else {
            // Everything model-written is shed: the allowance itself is too small.
            throw core::DataError(std::format("narrate_allowance {} is too small for the resolution record ({} tokens)",
                                              allowance, backend.CountTokens(text)));
        }
    }
}
```

In `src/turn/Session.cpp`, the narrate step's call becomes:

```cpp
    context::ExtendForNarrate(narratePrompt, resolved.text,
                              RenderNarrateMessage(proposal, resolution, {}, _settings.budget.narrateAllowance,
                                                   *_pBackend));
```

- [ ] **Step 8: Add the review parser and prompt**

Create `src/turn/Review.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"
#include "turn/Resolve.h"

namespace morpheus::turn {

// The review call's output, read (architecture spec, "Turn flow"). `consistent` and `problems` come from the
// narration check, `digest` from the compact transcript. Output that cannot be read leaves everything empty except
// `error`, which says why: a model mistake, so the turn goes on as if the review had found nothing.
struct Review {
    std::optional<bool> consistent = {};    // Empty when the check was off or the output could not be read.
    std::vector<std::string> problems = {}; // The contradictions; empty when consistent.
    std::string digest = {};                // Trimmed. Empty when the compact transcript was off.
    std::string error = {};
};

// Reads the output of a review call made with BuildReviewGrammar(check, digest): with `check`, a boolean
// "consistent" and an array of string "problems" that is empty exactly when "consistent" is true; with `digest`, a
// string "digest". Fields that were not asked for are ignored. Throws ContractError unless `check` or `digest` is set.
Review ParseReviewOutput(std::string_view text, bool check, bool digest);

// The review call's prompt, standalone: REVIEW_RULES as the system message, then one user message:
//   # Player
//   <input>
//
//   <RenderOutcome(proposal, resolution, {})>
//   # Narration
//   <narration>
//
//   # Task
//   <REVIEW_INSTRUCTION>
// stableCount is 1 (the rules).
inference::Prompt ReviewPrompt(const std::string& input, const ResolveProposal& proposal,
                               const Resolution& resolution, const std::string& narration);

} // namespace morpheus::turn
```

Create `src/turn/Review.cpp`:

```cpp
#include "turn/Review.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

Review ParseReviewOutput(std::string_view text, bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "ParseReviewOutput: needs the check, the digest or both");
    // Every unreadable output ends here: nothing is kept, only the reason.
    auto failed = [](std::string why) {
        Review review;
        review.error = std::move(why);
        return review;
    };
    // Parse without exceptions: a cut-off or malformed output is a model mistake, not an engine bug.
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return failed("the review output is not complete JSON; it may have hit review_max_tokens");
    }
    if (!json.is_object()) {
        return failed("the review output is not a JSON object");
    }

    Review review;
    if (check) {
        // The verdict, and its problems: listed exactly when the narration contradicts the outcome.
        if (!json.contains("consistent") || !json.at("consistent").is_boolean()) {
            return failed("'consistent' must be true or false");
        }
        const bool problemsReadable =
            json.contains("problems") && json.at("problems").is_array() &&
            std::all_of(json.at("problems").begin(), json.at("problems").end(),
                        [](const nlohmann::json& problem) { return problem.is_string(); });
        if (!problemsReadable) {
            return failed("'problems' must be an array of strings");
        }
        review.consistent = json.at("consistent").get<bool>();
        for (const nlohmann::json& problem : json.at("problems")) {
            review.problems.push_back(problem.get<std::string>());
        }
        if (*review.consistent != review.problems.empty()) {
            return failed("'problems' must list the contradictions exactly when 'consistent' is false");
        }
    }
    if (digest) {
        if (!json.contains("digest") || !json.at("digest").is_string()) {
            return failed("'digest' must be a string");
        }
        review.digest = core::Trim(json.at("digest").get<std::string>());
    }
    return review;
}

inference::Prompt ReviewPrompt(const std::string& input, const ResolveProposal& proposal,
                               const Resolution& resolution, const std::string& narration)
{
    // The rules, then what the reviewer judges: the input, the whole outcome (nothing shed: the review has no
    // allowance to fit) and the narration.
    inference::Prompt prompt;
    prompt.messages.push_back({inference::Role::System, std::string(REVIEW_RULES)});
    prompt.messages.push_back({inference::Role::User, "# Player\n" + input + "\n\n" +
                                                          RenderOutcome(proposal, resolution, {}) + "\n# Narration\n" +
                                                          narration + "\n\n# Task\n" +
                                                          std::string(REVIEW_INSTRUCTION)});
    prompt.stableCount = 1;
    return prompt;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, add `Review.cpp` after `Retrieval.cpp`:

```cmake
add_library(morpheus_turn STATIC
    Folding.cpp
    Replay.cpp
    Resolve.cpp
    ResolveGrammar.cpp
    Retrieval.cpp
    Review.cpp
    Session.cpp
    Worker.cpp
)
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar],[resolve],[review]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass. Turns still play as before: nothing calls the review yet, and the first narration's message is unchanged.

- [ ] **Step 10: Commit**

```bash
git add src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/turn/EngineText.h src/turn/Resolve.h src/turn/Resolve.cpp src/turn/Review.h src/turn/Review.cpp src/turn/CMakeLists.txt src/turn/Session.cpp tests/turn/ResolveGrammarTests.cpp tests/turn/ResolveTests.cpp tests/turn/ReviewTests.cpp
git commit -m "Add the review grammar, parser and prompt, and the renarrate correction" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
### Task 4: The session reviews each narration and renarrates a contradiction

After narrate, the session makes one review call when `narration_check` or `compact_transcript` is on. With the check on, a first review that says `"consistent": false` restarts the narration once: `onNarrationRestarted` fires, the narration is written again from the same narrate prompt plus the correction, and its own review decides the digest. The second narration is committed whatever its review says. A cancel during the review or the renarrate cancels the turn.

**Files:**
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Test: `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `turn::BuildReviewGrammar`, `turn::ParseReviewOutput`, `turn::ReviewPrompt`, `turn::Review`, `turn::RenderNarrateMessage(..., problems, ...)` (Task 3); `core::Settings::narrationCheck`, `compactTranscript`, `budget.reviewMaxTokens`, `budget.reviewThinkingTokens`, `core::CallKind::Review`, `core::CallKind::Renarrate` (Task 1); `core::TurnRecord::digest` (Task 2).
- Produces:
  - `constexpr int turn::REVIEW_RETRY_OFFSET = 1 << 23` (`turn/Session.h`).
  - `std::function<void(const std::vector<std::string>& problems)> turn::TurnCallbacks::onNarrationRestarted`.
  - Telemetry: `timings["review"]` (array; each entry is the call's `Telemetry` plus `consistent`, `problems`, `digest`, `error` when set); `timings["renarrate"]` (the renarrate call's `Telemetry` plus `"replaced"`: the first narration), only after a renarrate. `timings["narrate"]` stays the first narration's call.
  - `record.digest`: the last review's digest.

- [ ] **Step 1: Write the failing tests**

In `tests/turn/SessionTests.cpp`, add to the includes:

```cpp
#include "core/Dice.h"
#include "turn/ResolveGrammar.h"
```

Inside the anonymous namespace, after `Tail`, add:

```cpp
// A review that finds nothing wrong, with the turn's digest.
const std::string CONSISTENT = R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";

// TestSettings with the review call on: the narration check and the digest as given, and the review's budget from
// settings.example.json.
core::Settings ReviewSettings(bool check, bool digest)
{
    core::Settings settings = TestSettings();
    settings.narrationCheck = check;
    settings.compactTranscript = digest;
    settings.budget.reviewMaxTokens = 384;
    return settings;
}

// A scripted model for review tests. Resolve calls get `resolve`; review calls (their system message is REVIEW_RULES)
// get `reviews` in order; summary calls get "Summary."; narrate calls get `narrations` in order. Running out of
// replies is a test bug. The script must outlive the backends that call it.
struct ReviewScript {
    std::string resolve = {};
    std::vector<std::string> narrations = {};
    std::vector<std::string> reviews = {};
    std::size_t narrated = 0;
    std::size_t reviewed = 0;

    test::ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            if (spec.grammar.find("intent") != std::string::npos) {
                return resolve;
            }
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                MORPHEUS_REQUIRE(reviewed < reviews.size(), "ReviewScript: no review reply left");
                return reviews[reviewed++];
            }
            const std::string& last = prompt.messages.back().content;
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
                return "Summary.";
            }
            MORPHEUS_REQUIRE(narrated < narrations.size(), "ReviewScript: no narration left");
            return narrations[narrated++];
        };
    }
};
```

Append the test cases:

```cpp
TEST_CASE("A consistent review commits the narration and its digest", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {CONSISTENT}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_consistent"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest == "The player walked to the lighthouse.");
    CHECK(session.History().at(0).digest == "The player walked to the lighthouse."); // Stored with the turn.
    CHECK(restarts == 0);

    // The review is a standalone call: the rules, then the input, the outcome and the narration.
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    const inference::Prompt& review = calls[2].prompt;
    REQUIRE(review.messages.size() == 2);
    CHECK(review.messages[0].content == turn::REVIEW_RULES);
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Player\nI walk to the lighthouse.\n"));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("Applied: player.location = \"lighthouse\""));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Narration\nYou walk to the lighthouse.\n"));
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(true, true));
    CHECK(calls[2].spec.maxTokens == 384);
    CHECK(calls[2].spec.thinkingMaxTokens == 0);
    CHECK(calls[2].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Review, 0));

    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 1);
    CHECK(reviews[0].at("consistent") == true);
    CHECK(reviews[0].at("digest") == "The player walked to the lighthouse.");
    CHECK(reviews[0].contains("prompt_tokens"));
    CHECK_FALSE(reviews[0].contains("problems"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("A narration that contradicts the outcome is narrated once more and replaced", "[session][review]")
{
    const std::string contradicts =
        R"({"consistent":false,"problems":["The climb failed, but the narration has the player reach the top."],)"
        R"("digest":"The player reached the top."})";
    const std::string fixed = R"({"consistent":true,"problems":[],"digest":"The player slipped on the cliff."})";
    ReviewScript script{CLIMB, {"You reach the top with ease.", "You slip on the wet rock."}, {contradicts, fixed}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate"), 42, backend);
    std::vector<std::vector<std::string>> restarts;
    std::string streamed;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>& problems) { restarts.push_back(problems); };
    callbacks.onNarrationToken = [&](std::string_view piece) { streamed += piece; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I climb the cliff.", callbacks, {});
    REQUIRE(record);

    // The second narration and the second review's digest are committed.
    CHECK(record->narration == "You slip on the wet rock.");
    CHECK(record->digest == "The player slipped on the cliff.");
    REQUIRE(restarts.size() == 1);
    CHECK(restarts[0] == std::vector<std::string>{"The climb failed, but the narration has the player reach the top."});
    CHECK(streamed == "You reach the top with ease.You slip on the wet rock."); // Both stream; the UI clears on the restart.

    // The renarrate is the narrate prompt with the correction before the instruction, with its own seed.
    const auto& calls = backend.Calls(); // resolve, narrate, review, renarrate, review
    REQUIRE(calls.size() == 5);
    const inference::Prompt& first = calls[1].prompt;
    const inference::Prompt& again = calls[3].prompt;
    REQUIRE(again.messages.size() == first.messages.size());
    for (std::size_t i = 0; i + 1 < first.messages.size(); ++i) {
        CHECK(again.messages[i].content == first.messages[i].content);
    }
    CHECK_THAT(first.messages.back().content, !ContainsSubstring(std::string(turn::RENARRATE_CORRECTION)));
    CHECK_THAT(again.messages.back().content,
               ContainsSubstring(std::string(turn::RENARRATE_CORRECTION) +
                                 "\n- The climb failed, but the narration has the player reach the top.\n\n" +
                                 std::string(turn::NARRATE_INSTRUCTION)));
    CHECK(calls[3].spec.grammar.empty());
    CHECK(calls[3].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Renarrate, 0));
    // The second review reads the second narration, seeded apart from any first review.
    CHECK_THAT(calls[4].prompt.messages[1].content, ContainsSubstring("# Narration\nYou slip on the wet rock.\n"));
    CHECK(calls[4].spec.sampling.seed ==
          core::SamplingSeed(42, 1, core::CallKind::Review, turn::REVIEW_RETRY_OFFSET));

    // Both verdicts, the renarrate's call and the replaced text are in the telemetry.
    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 2);
    CHECK(reviews[0].at("consistent") == false);
    CHECK(reviews[0].at("problems") ==
          nlohmann::json::array({"The climb failed, but the narration has the player reach the top."}));
    CHECK(reviews[1].at("consistent") == true);
    CHECK(record->timings.at("renarrate").at("replaced") == "You reach the top with ease.");
    CHECK(record->timings.at("renarrate").contains("generated"));
    CHECK(record->timings.at("narrate").contains("generated")); // The first narration's call stays.
}

TEST_CASE("A renarrated turn is committed whatever its second review says", "[session][review]")
{
    const std::string first = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    const std::string second =
        R"({"consistent":false,"problems":["The player still flies."],"digest":"The player flew again."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You float to the lighthouse."}, {first, second}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate_twice"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You float to the lighthouse.");
    CHECK(record->digest == "The player flew again.");
    CHECK(restarts == 1);
    CHECK(backend.Calls().size() == 5); // No second renarrate.
    CHECK(record->timings.at("review").at(1).at("consistent") == false);
}

TEST_CASE("An unreadable review commits the narration with no digest and says why", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {R"({"consistent":tr)"}}; // Cut off.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_unreadable"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest.empty());
    CHECK(backend.Calls().size() == 3); // As if the review had found nothing: no renarrate.
    const nlohmann::json& review = record->timings.at("review").at(0);
    CHECK_THAT(review.at("error").get<std::string>(), ContainsSubstring("not complete JSON"));
    CHECK_FALSE(review.contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With only the compact transcript on, the review writes the digest and never renarrates", "[session][review]")
{
    // The output says the narration contradicts the outcome, but the check is off: only the digest is read.
    ReviewScript script{MOVE, {"You fly to the lighthouse."},
                        {R"({"consistent":false,"problems":["The player flies."],"digest":"The player went to the lighthouse."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_review_digest_only"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(false, true));
    CHECK(record->narration == "You fly to the lighthouse.");
    CHECK(record->digest == "The player went to the lighthouse.");
    CHECK_FALSE(record->timings.at("review").at(0).contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With both switches off no review call is made", "[session][review]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, false),
                                                 NewRunFile("session_review_off"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(backend.Calls().size() == 2); // resolve, narrate
    CHECK_FALSE(record->timings.contains("review"));
    CHECK(record->digest.empty());
}

TEST_CASE("A cancel during the review or the renarrate commits nothing", "[session][review]")
{
    const std::string contradicts = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You walk to the lighthouse."}, {contradicts, CONSISTENT}};
    const test::ScriptedBackend::Responder scripted = script.Responder();
    std::stop_source stop;
    turn::TurnCallbacks callbacks;

    SECTION("during the review")
    {
        // The stop arrives as the review call starts, so its reply never streams.
        test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                stop.request_stop();
            }
            return scripted(prompt, spec);
        });
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_review_cancel"), 42, backend);
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
    SECTION("during the renarrate")
    {
        test::ScriptedBackend backend(16384, scripted);
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_renarrate_cancel"), 42, backend);
        callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { stop.request_stop(); };
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(backend.Calls().size() == 4); // resolve, narrate, review, and the cancelled renarrate.
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `onNarrationRestarted` is not a member of `morpheus::turn::TurnCallbacks`, and `REVIEW_RETRY_OFFSET` is not a member of `morpheus::turn`.

- [ ] **Step 3: Declare the callback and the offset**

In `src/turn/Session.h`, after `constexpr int CONDENSE_SUMMARY_DIVISOR = 2; ...`, add:

```cpp
// Added to a turn's attempt for the seed of its second review, so that seed never equals a regenerated turn's first
// review seed (attempt + 1). PlayTurn requires attempt < REVIEW_RETRY_OFFSET, which keeps the sum within
// SamplingSeed's limit of 2^24.
constexpr int REVIEW_RETRY_OFFSET = 1 << 23;
```

Replace `TurnCallbacks` with:

```cpp
struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    // Every narration's pieces as they are written, the renarrate's included.
    inference::TokenCallback onNarrationToken = {};
    // The review found that the narration contradicts the outcome (`problems` are its reasons): the narration starts
    // over, and the new one streams through onNarrationToken. At most once per turn.
    std::function<void(const std::vector<std::string>& problems)> onNarrationRestarted = {};
};
```

Replace the `RunTurn` comment's first sentence with:

```cpp
    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, (narration_check or
    // compact_transcript: review, and at most one renarrate with its own review), commit. Control characters other
```

(the rest of that comment is unchanged).

- [ ] **Step 4: Review, renarrate and commit**

In `src/turn/Session.cpp`, add `#include "turn/Review.h"` after `#include "turn/ResolveGrammar.h"`.

At the top of `Session::PlayTurn`, before `const int turn = _store.LatestTurn() + 1;`, add:

```cpp
    // A second review seeds with attempt + REVIEW_RETRY_OFFSET, which must stay within SamplingSeed's range.
    MORPHEUS_REQUIRE(attempt >= 0 && attempt < REVIEW_RETRY_OFFSET,
                     "PlayTurn: attempt must be in 0..REVIEW_RETRY_OFFSET-1");
```

Replace everything from the line `    // 4. Narrate: the resolve prompt plus the resolve output and the resolution record, so all of it stays cached.` through the line `    telemetry["narrate"] = Telemetry(narrated, narrateSpec);` with:

```cpp
    // 4. Narrate: the resolve prompt plus the resolve output and the narrate message, so all of the resolve prompt
    // stays cached. `problems` is empty for the first narration; a renarrate passes the review's, which the narrate
    // message shows before its instruction. Both stream through onNarrationToken.
    inference::GenerationSpec narrateSpec;
    narrateSpec.maxTokens = _settings.budget.narrateMaxTokens;
    narrateSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    auto narrate = [&](const std::vector<std::string>& problems, core::CallKind kind) {
        inference::Prompt narratePrompt = assembly->prompt;
        context::ExtendForNarrate(narratePrompt, resolved.text,
                                  RenderNarrateMessage(proposal, resolution, problems,
                                                       _settings.budget.narrateAllowance, *_pBackend));
        inference::GenerationSpec spec = narrateSpec;
        spec.sampling =
            inference::SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, kind, attempt));
        return _pBackend->Complete(narratePrompt, spec, callbacks.onNarrationToken, stop);
    };
    const inference::Completion narrated = narrate({}, core::CallKind::Narrate);
    if (narrated.cancelled) {
        return std::nullopt;
    }
    record.narration = core::Trim(narrated.text);
    telemetry["narrate"] = Telemetry(narrated, narrateSpec);

    // 5. Review, when the narration check or the compact transcript is on: one standalone call reads the outcome
    // and the narration, and says whether they agree (the check), writes the turn's digest (the compact transcript),
    // or both.
    const bool check = _settings.narrationCheck;
    const bool digest = _settings.compactTranscript;
    if (check || digest) {
        inference::GenerationSpec reviewSpec;
        reviewSpec.grammar = BuildReviewGrammar(check, digest);
        reviewSpec.maxTokens = _settings.budget.reviewMaxTokens;
        reviewSpec.thinkingMaxTokens = _settings.budget.reviewThinkingTokens;
        telemetry["review"] = nlohmann::json::array();
        // Reviews `narration` with the sampling seed of `seedAttempt` and adds the call to timings.review; nullopt
        // when cancelled. Unreadable output is a model mistake: its Review holds only the error, so it finds nothing.
        auto review = [&](const std::string& narration, int seedAttempt) -> std::optional<Review> {
            inference::GenerationSpec spec = reviewSpec;
            spec.sampling = inference::SamplingFrom(
                _settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Review, seedAttempt));
            const inference::Completion reviewed =
                _pBackend->Complete(ReviewPrompt(input, proposal, resolution, narration), spec, {}, stop);
            if (reviewed.cancelled) {
                return std::nullopt;
            }
            Review result = ParseReviewOutput(reviewed.text, check, digest);
            nlohmann::json entry = Telemetry(reviewed, spec);
            if (result.consistent) {
                entry["consistent"] = *result.consistent;
            }
            if (!result.problems.empty()) {
                entry["problems"] = result.problems;
            }
            if (!result.digest.empty()) {
                entry["digest"] = result.digest;
            }
            if (!result.error.empty()) {
                entry["error"] = result.error;
            }
            telemetry["review"].push_back(std::move(entry));
            return result;
        };

        std::optional<Review> verdict = review(record.narration, attempt);
        if (!verdict) {
            return std::nullopt;
        }
        if (verdict->consistent == false) {
            // 6. Renarrate, once: the narration contradicts the outcome. The player sees it start over, the new
            // narration streams, and its own review decides the digest. Whatever that review says, the second
            // narration is committed: there is no second renarrate.
            if (callbacks.onNarrationRestarted) {
                callbacks.onNarrationRestarted(verdict->problems);
            }
            const inference::Completion renarrated = narrate(verdict->problems, core::CallKind::Renarrate);
            if (renarrated.cancelled) {
                return std::nullopt;
            }
            telemetry["renarrate"] = Telemetry(renarrated, narrateSpec);
            telemetry["renarrate"]["replaced"] = record.narration;
            record.narration = core::Trim(renarrated.text);
            // The offset keeps this seed apart from a regenerated turn's first review (attempt + 1).
            verdict = review(record.narration, attempt + REVIEW_RETRY_OFFSET);
            if (!verdict) {
                return std::nullopt;
            }
        }
        record.digest = verdict->digest;
    }

    // 7. Commit.
    telemetry["resolve"] = Telemetry(resolved, resolveSpec);
    if (!proposal.error.empty()) {
        // Why nothing was proposed: the record's resolve output alone does not say so for JSON of the wrong shape.
        telemetry["resolve"]["error"] = proposal.error;
    }
```

The lines after it (`telemetry["retrieval"] = ...` through `return record;`) stay as they are.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session]"`
Expected: all pass, the new `[review]` cases included. The older session tests build their settings in code (`TestSettings`), with both switches off, so their call counts are unchanged.

Then: `ctest --preset tests`
Expected: all pass. The app tests copy `settings.example.json` (both switches on), so their turns now make a review call; their responders answer it with narration text, which reads as an unreadable review (a model mistake), so their turns still commit.

- [ ] **Step 6: Commit**

```bash
git add src/turn/Session.h src/turn/Session.cpp tests/turn/SessionTests.cpp
git commit -m "Review each narration and narrate a contradiction once more" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

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
### Task 6: The compact transcript

Older turns appear in the transcript layer (L3) as their digest. The newest `prose_turns` turns keep their narration, and an older turn without a digest (played with `compact_transcript` off, or whose review failed) keeps it too. User messages are unchanged. The resolve prompt is assembled by `context::AssembleResolvePrompt`, so `ContextInputs` carries the number, which `Session::Inputs` sets; fold calls always render full narration. `ENGINE_RULES` tells the model why older turns look short.

**Files:**
- Modify: `src/context/Transcript.h`, `src/context/Transcript.cpp`, `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp`, `src/turn/Folding.cpp`, `src/turn/Session.cpp`, `src/turn/EngineText.h`
- Test: `tests/context/RenderTests.cpp`, `tests/context/PromptBuilderTests.cpp`, `tests/turn/FoldingTests.cpp`, `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `core::TurnRecord::digest` (Task 2); `core::Settings::compactTranscript`, `proseTurns` (Task 1); `ReviewScript` and `ReviewSettings` in `SessionTests.cpp` (Task 4).
- Produces:
  - `std::vector<inference::Message> context::RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns)` (throws `ContractError` for `proseTurns < 1`).
  - `int context::ContextInputs::proseTurns = std::numeric_limits<int>::max()` (after `transcript`).

- [ ] **Step 1: Write the failing render tests**

In `tests/context/RenderTests.cpp`, add to the includes:

```cpp
#include <cstddef>
#include <format>
#include <limits>
#include <vector>

#include "core/Errors.h"
```

In "RenderTranscript turns each turn into a user and an assistant message", replace the call with:

```cpp
    const std::vector<inference::Message> messages =
        context::RenderTranscript({TurnWithMechanics(), quiet}, std::numeric_limits<int>::max());
```

Inside the anonymous namespace, after `TurnWithMechanics`, add:

```cpp
// Turns 1..count, each with an input, a narration and a digest that name its number.
std::vector<core::TurnRecord> NumberedTurns(int count)
{
    std::vector<core::TurnRecord> turns;
    for (int number = 1; number <= count; ++number) {
        core::TurnRecord turn;
        turn.number = number;
        turn.input = std::format("Input {}.", number);
        turn.narration = std::format("Narration {}.", number);
        turn.digest = std::format("Digest {}.", number);
        turns.push_back(turn);
    }
    return turns;
}
```

Append:

```cpp
TEST_CASE("RenderTranscript shows older turns as their digest", "[context]")
{
    std::vector<core::TurnRecord> turns = NumberedTurns(4);
    turns[1].digest.clear(); // Turn 2 has no digest: played with compact_transcript off, or its review failed.

    // proseTurns 2: turns 3 and 4 keep their narration; turns 1 and 2 are older.
    const std::vector<inference::Message> messages = context::RenderTranscript(turns, 2);
    REQUIRE(messages.size() == 8);
    CHECK(messages[0].content == "Input 1."); // User messages never change.
    CHECK(messages[1].content == "Digest 1.");
    CHECK(messages[3].content == "Narration 2."); // An older turn without a digest keeps its narration.
    CHECK(messages[4].content == "Input 3.");
    CHECK(messages[5].content == "Narration 3.");
    CHECK(messages[7].content == "Narration 4.");

    // Without a limit every turn shows its narration.
    const std::vector<inference::Message> full = context::RenderTranscript(turns, std::numeric_limits<int>::max());
    CHECK(full[1].content == "Narration 1.");
    CHECK_THROWS_AS(context::RenderTranscript(turns, 0), core::ContractError);
}

TEST_CASE("A compact transcript keeps its prefix from one turn to the next", "[context]")
{
    constexpr std::size_t PROSE_TURNS = 2;
    const std::vector<core::TurnRecord> turns = NumberedTurns(6);
    for (std::size_t n = 1; n < turns.size(); ++n) {
        INFO("turns 1.." << n << " and 1.." << n + 1);
        const std::vector<inference::Message> before = context::RenderTranscript(
            std::vector<core::TurnRecord>(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(n)),
            static_cast<int>(PROSE_TURNS));
        const std::vector<inference::Message> after = context::RenderTranscript(
            std::vector<core::TurnRecord>(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(n + 1)),
            static_cast<int>(PROSE_TURNS));
        // Every entry of the turns before turn n + 1 - PROSE_TURNS is the same in both...
        const std::size_t stableTurns = n > PROSE_TURNS ? n - PROSE_TURNS : 0;
        for (std::size_t i = 0; i < 2 * stableTurns; ++i) {
            CHECK(before[i].content == after[i].content);
        }
        // ...and turn n + 1 - PROSE_TURNS itself changes form: narration before, digest after.
        if (n >= PROSE_TURNS) {
            const std::size_t assistant = 2 * (n + 1 - PROSE_TURNS) - 1;
            CHECK(before[assistant].content != after[assistant].content);
        }
    }
}
```

- [ ] **Step 2: Write the failing prompt, fold and session tests**

Append to `tests/context/PromptBuilderTests.cpp`:

```cpp
TEST_CASE("Digests shrink the transcript layer", "[context]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(8192);
    context::ContextInputs inputs = Inputs(world);
    inputs.transcript.clear();
    for (int number = 5; number <= 7; ++number) {
        core::TurnRecord turn;
        turn.number = number;
        turn.input = "I walk on.";
        turn.narration = std::string(400, 'n');
        turn.digest = "The player walked on.";
        inputs.transcript.push_back(turn);
    }
    const context::Assembly full = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);
    inputs.proseTurns = 1;
    const context::Assembly compact = context::AssembleResolvePrompt(world, inputs, RoomyBudget(), backend);

    CHECK(compact.tokens.transcript < full.tokens.transcript);
    CHECK(compact.transcriptCost < full.transcriptCost);
    REQUIRE(compact.prompt.messages.size() == 1 + 6 + 1);
    CHECK(compact.prompt.messages[2].content == "The player walked on.");
    CHECK(compact.prompt.messages[4].content == "The player walked on.");
    CHECK(compact.prompt.messages[6].content == std::string(400, 'n')); // The newest turn keeps its narration.
    CHECK(full.prompt.messages[2].content == std::string(400, 'n'));     // The default shows every narration.
}
```

Append to `tests/turn/FoldingTests.cpp`:

```cpp
TEST_CASE("A fold sees the full narration even of turns with a digest", "[folding]")
{
    core::TurnRecord walked = Turn(1, "I walk.", "You walk the long shore road.");
    walked.digest = "The player walked.";
    const inference::Prompt prompt =
        turn::SummarizePrompt({inference::Role::System, "story"}, {walked, Turn(2, "I wait.", "Time passes.")});
    REQUIRE(prompt.messages.size() == 1 + 4 + 1);
    CHECK(prompt.messages[2].content == "You walk the long shore road.");
    CHECK(prompt.messages[4].content == "Time passes.");
}
```

Append to `tests/turn/SessionTests.cpp`:

```cpp
TEST_CASE("With compact_transcript on, older turns reach the prompt as their digest", "[session][review]")
{
    const fs::path file = NewRunFile("session_compact");
    {
        core::Settings settings = ReviewSettings(false, true);
        settings.proseTurns = 1;
        ReviewScript script{MOVE,
                            {"Narration one.", "Narration two.", "Narration three."},
                            {R"({"digest":"Digest one."})", R"({"digest":"Digest two."})", R"({"digest":"Digest three."})"}};
        test::ScriptedBackend backend(16384, script.Responder());
        turn::Session session = turn::Session::Start(MiniWorld(), settings, file, 42, backend);
        for (const char* input : {"I walk.", "I walk on.", "I walk further."}) {
            REQUIRE(session.RunTurn(input, {}, {}));
        }

        // The third turn's resolve prompt: turn 1 as its digest, turn 2 (the newest) as its narration.
        const auto& calls = backend.Calls(); // Per turn: resolve, narrate, review.
        REQUIRE(calls.size() == 9);
        const inference::Prompt& third = calls[6].prompt;
        REQUIRE(third.messages.size() == 1 + 4 + 1);
        CHECK(third.messages[1].content.starts_with("I walk.")); // The input stays.
        CHECK(third.messages[2].content == "Digest one.");
        CHECK(third.messages[4].content == "Narration two.");
        CHECK_THAT(third.messages[0].content, ContainsSubstring("Older turns in the transcript show a short summary"));
        // The history keeps the full narration (the Play window shows it).
        CHECK(session.History().at(0).narration == "Narration one.");
    }

    // With compact_transcript off, the same run renders every turn in full, its stored digests notwithstanding.
    test::ScriptedBackend backend(16384, Responder(MOVE));
    turn::Session session = turn::Session::Load(MiniWorld(), TestSettings(), file, backend);
    REQUIRE(session.RunTurn("I stop.", {}, {}));
    const inference::Prompt& fourth = backend.Calls().at(0).prompt;
    REQUIRE(fourth.messages.size() == 1 + 6 + 1);
    CHECK(fourth.messages[2].content == "Narration one.");
    CHECK(fourth.messages[4].content == "Narration two.");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `RenderTranscript` does not take two arguments, and `proseTurns` is not a member of `morpheus::context::ContextInputs`.

- [ ] **Step 4: Render digests for older turns**

In `src/context/Transcript.h`, replace the `RenderTranscript` comment and declaration with:

```cpp
// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one) and
// an assistant message: the narration for the newest `proseTurns` turns; for an older turn its digest when that is
// not empty, else its narration. std::numeric_limits<int>::max() shows every narration (fold calls, and the session
// with compact_transcript off). Throws ContractError for proseTurns < 1.
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns);
```

In `src/context/Transcript.cpp`, add `#include "core/Errors.h"` to the includes, and replace `RenderTranscript` with:

```cpp
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    MORPHEUS_REQUIRE(proseTurns >= 1, "RenderTranscript: proseTurns must be at least 1");
    // Turns from this index on are the newest proseTurns; the ones before it are older.
    const std::size_t prose = static_cast<std::size_t>(proseTurns);
    const std::size_t firstProse = turns.size() > prose ? turns.size() - prose : 0;

    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const core::TurnRecord& turn = turns[i];
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        // An older turn shows its digest. One without (played with compact_transcript off, or its review failed)
        // keeps its narration.
        const bool digested = i < firstProse && !turn.digest.empty();
        messages.push_back({inference::Role::Assistant, digested ? turn.digest : turn.narration});
    }
    return messages;
}
```

- [ ] **Step 5: Pass the number through the prompt builder, the session and folding**

In `src/context/PromptBuilder.h`, add `#include <limits>` to the includes, and in `ContextInputs` after the `transcript` member add:

```cpp
    // L3: the newest turns that show their narration; older ones show their digest (context::RenderTranscript).
    // The default shows every narration.
    int proseTurns = std::numeric_limits<int>::max();
```

In `src/context/PromptBuilder.cpp`, replace `for (inference::Message& message : RenderTranscript(inputs.transcript)) {` with:

```cpp
    for (inference::Message& message : RenderTranscript(inputs.transcript, inputs.proseTurns)) {
```

In `src/turn/Folding.cpp`, add `#include <limits>` to the includes, and replace the first lines of `SummarizePrompt`'s body with:

```cpp
    // The system message, then the turns as the transcript renders them (stable, so cached), then the instruction.
    // Summaries are written from the full narration, digests or not.
    inference::Prompt prompt;
    prompt.messages.push_back(systemMessage);
    for (inference::Message& message : context::RenderTranscript(turns, std::numeric_limits<int>::max())) {
```

In `src/turn/Session.cpp`, add `#include <limits>` to the includes, and in `Session::Inputs` after `inputs.state = state;` add:

```cpp
    // With the compact transcript on, older turns show their digest; otherwise every turn shows its narration.
    inputs.proseTurns = _settings.compactTranscript ? _settings.proseTurns : std::numeric_limits<int>::max();
```

- [ ] **Step 6: Tell the model about digests**

In `src/turn/EngineText.h`, replace the last line of `ENGINE_RULES`

```cpp
In the transcript, the bracketed line after a player's input is the engine's record of that turn: rolls first, then applied changes.)";
```

with

```cpp
In the transcript, the bracketed line after a player's input is the engine's record of that turn: rolls first, then applied changes. Older turns in the transcript show a short summary of what was narrated rather than the narration itself; narrate in full prose regardless.)";
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[context],[folding],[session]"`
Expected: all pass. (`ENGINE_RULES` grows by about 35 FakeBackend tokens; the folding tests' comments say two turns fit the transcript budget and three do not, which still holds.)

Then: `ctest --preset tests`
Expected: all pass, the replays included (their fixtures turn the compact transcript on; summaries are exempt from the replay's checks, so where folds land does not matter).

- [ ] **Step 8: Commit**

```bash
git add src/context/Transcript.h src/context/Transcript.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Folding.cpp src/turn/Session.cpp src/turn/EngineText.h tests/context/RenderTests.cpp tests/context/PromptBuilderTests.cpp tests/turn/FoldingTests.cpp tests/turn/SessionTests.cpp
git commit -m "Show older transcript turns as their digest" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The world's scene block

A world may declare which state values describe where the player is (`paths`) and whose location others are compared with (`present_at`). The loader validates it against the schema and the initial state. Sandcrawler declares it; the mini test world does not.

**Files:**
- Modify: `src/core/World.h`, `src/core/World.cpp`, `game_projects/sandcrawler/world.json`
- Test: `tests/core/WorldTests.cpp`, `tests/core/SandcrawlerWorldTests.cpp`

**Interfaces:**
- Consumes: `core::StateSchema::FindEntityType`, `core::FieldType::String`.
- Produces: `struct core::SceneDecl { std::vector<std::string> paths; std::string presentAt; }`; `std::optional<core::SceneDecl> core::World::scene` (after `stats`). Every path is `entity.field` with exactly one dot, its entity in the initial state, its field a `string` field of that entity's type; `presentAt` is one of the paths.

- [ ] **Step 1: Write the failing tests**

Append to `tests/core/WorldTests.cpp`:

```cpp
TEST_CASE("LoadWorld reads an optional scene block", "[world]")
{
    CHECK_FALSE(LoadWorld(MiniWorldDir()).scene); // The mini world declares none.

    const fs::path dir = CopyMiniWorld("world_scene");
    morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("scene", R"({"paths": ["player.location"], "present_at": "player.location"})"));
    const World world = LoadWorld(dir);
    REQUIRE(world.scene);
    CHECK(world.scene->paths == std::vector<std::string>{"player.location"});
    CHECK(world.scene->presentAt == "player.location");
}

TEST_CASE("LoadWorld names world.json and the value at fault in a bad scene block", "[world]")
{
    // Each case: the scene block, and the part of the message that names what is wrong.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {R"("here")", "'scene' must be an object"},
        {R"({"paths": ["player.location"], "present_at": "player.location", "radius": 3})", "'radius'"},
        {R"({"paths": [], "present_at": "player.location"})", "'paths'"},
        {R"({"present_at": "player.location"})", "'paths'"},
        {R"({"paths": [7], "present_at": "player.location"})", "'paths'"},
        {R"({"paths": ["location"], "present_at": "location"})", "'location'"},
        {R"({"paths": ["player.location.x"], "present_at": "player.location.x"})", "'player.location.x'"},
        {R"({"paths": ["keeper.location"], "present_at": "keeper.location"})", "'keeper'"},
        {R"({"paths": ["player.health"], "present_at": "player.health"})", "'player.health'"},
        {R"({"paths": ["player.mood"], "present_at": "player.mood"})", "'player.mood'"},
        {R"({"paths": ["player.location"], "present_at": "npc.location"})", "'npc.location'"},
        {R"({"paths": ["player.location"]})", "'present_at'"},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& [scene, named] = cases[i];
        INFO("scene " << scene);
        const fs::path dir = CopyMiniWorld("world_scene_bad_" + std::to_string(i));
        morpheus::test::WriteFile(dir / "world.json", MiniWorldJsonWith("scene", scene));
        CHECK_THROWS_AS(LoadWorld(dir), DataError);
        CHECK_THROWS_WITH(LoadWorld(dir), ContainsSubstring("world.json") && ContainsSubstring(named));
    }
}
```

Add `#include <utility>` to the includes of `tests/core/WorldTests.cpp`.

In `tests/core/SandcrawlerWorldTests.cpp`, before the closing brace of "The shipped sandcrawler world loads", add:

```cpp
    // Scene retrieval: where the captain and the crawler are, and who is at the captain's location.
    REQUIRE(world.scene);
    CHECK(world.scene->paths == std::vector<std::string>{"player.location", "crawler.location"});
    CHECK(world.scene->presentAt == "player.location");
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `scene` is not a member of `morpheus::core::World`.

- [ ] **Step 3: Declare the scene**

In `src/core/World.h`, add `#include <optional>` to the includes, and after `LoreEntry` add:

```cpp
// world.json's optional "scene": the state values that describe where the player is, for scene retrieval. Each path
// is "entity.field", a string field of an entity of the initial state; presentAt is one of them: every other entity
// whose field of that name holds the same value is present (turn::SceneOf).
struct SceneDecl {
    std::vector<std::string> paths = {};
    std::string presentAt = {};
};
```

In `World`, after the `stats` member, add:

```cpp
    std::optional<SceneDecl> scene = {}; // Absent: retrieval searches with the input and the previous digest only.
```

Replace the `LoadWorld` comment with:

```cpp
// Loads a world folder: world.json, rules.md, lore.csv, state_schema.json, initial_state.json.
// Throws DataError naming the file (and field) at fault. world.json's optional "scene" is checked against the schema
// and the initial state.
```

- [ ] **Step 4: Load and validate it**

In `src/core/World.cpp`, add `#include <utility>` to the includes. Then:

Before the `// world.json: identity, dice, difficulties, defaults.` block's `{`, after `const std::string worldWhere = ...;`, add:

```cpp
    nlohmann::json sceneJson = nullptr; // world.json's "scene", checked once the schema and the state are loaded.
```

Replace the `allowedKeys` line with:

```cpp
        const std::set<std::string> allowedKeys = {"id",           "title",       "dice",       "check_entity",
                                                   "stats",        "difficulties", "memory_mode", "scene"};
```

At the end of that block (after the `difficulties` loop, before the block's closing `}`), add:

```cpp
        if (json.contains("scene")) {
            sceneJson = json.at("scene");
        }
```

After the stats loop at the end of the function, before `return world;`, add:

```cpp
    // scene: optional. Its paths name string fields of entities that exist from the start, and present_at is one of
    // them, so SceneOf can read every value each turn.
    if (!sceneJson.is_null()) {
        if (!sceneJson.is_object()) {
            throw DataError(worldWhere + ": 'scene' must be an object");
        }
        for (auto it = sceneJson.begin(); it != sceneJson.end(); ++it) {
            if (it.key() != "paths" && it.key() != "present_at") {
                throw DataError(std::format("{}: unknown key '{}' in 'scene'", worldWhere, it.key()));
            }
        }
        if (!sceneJson.contains("paths") || !sceneJson.at("paths").is_array() || sceneJson.at("paths").empty()) {
            throw DataError(worldWhere + ": 'scene' needs a non-empty 'paths' array");
        }
        SceneDecl scene;
        for (const nlohmann::json& entry : sceneJson.at("paths")) {
            if (!entry.is_string()) {
                throw DataError(worldWhere + ": 'scene' 'paths' entries must be strings");
            }
            const std::string path = entry.get<std::string>();
            // Exactly "entity.field": one dot, with text on both sides.
            const std::size_t dot = path.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == path.size() ||
                path.find('.', dot + 1) != std::string::npos) {
                throw DataError(std::format("{}: scene path '{}' must be 'entity.field'", worldWhere, path));
            }
            const std::string entity = path.substr(0, dot);
            const std::string field = path.substr(dot + 1);
            if (!world.initialState.contains(entity)) {
                throw DataError(std::format("{}: scene path '{}': entity '{}' is not in initial_state.json", worldWhere,
                                            path, entity));
            }
            const EntityTypeDef* pType =
                world.schema.FindEntityType(world.initialState.at(entity).at("type").get<std::string>());
            MORPHEUS_REQUIRE(pType != nullptr, "LoadWorld: scene entity type missing after ValidateState");
            const auto fieldIt = pType->fields.find(field);
            if (fieldIt == pType->fields.end() || fieldIt->second.type != FieldType::String) {
                throw DataError(std::format("{}: scene path '{}': '{}' is not a string field of entity '{}'",
                                            worldWhere, path, field, entity));
            }
            scene.paths.push_back(path);
        }
        if (!sceneJson.contains("present_at") || !sceneJson.at("present_at").is_string()) {
            throw DataError(worldWhere + ": 'scene' needs a string 'present_at'");
        }
        scene.presentAt = sceneJson.at("present_at").get<std::string>();
        if (std::find(scene.paths.begin(), scene.paths.end(), scene.presentAt) == scene.paths.end()) {
            throw DataError(std::format("{}: scene 'present_at' '{}' is not one of 'paths'", worldWhere, scene.presentAt));
        }
        world.scene = std::move(scene);
    }
```

- [ ] **Step 5: Declare sandcrawler's scene**

Replace `game_projects/sandcrawler/world.json` with:

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "dice": "1d20+stat",
  "check_entity": "player",
  "stats": ["grit", "wits", "presence", "agility"],
  "difficulties": { "easy": 8, "medium": 12, "hard": 16, "extreme": 20 },
  "memory_mode": "engine",
  "scene": {
    "paths": ["player.location", "crawler.location"],
    "present_at": "player.location"
  }
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[world]"`
Expected: all pass, `[sandcrawler]` included.

Then: `ctest --preset tests`
Expected: all pass (the sandcrawler replay loads the edited world; nothing reads the scene yet).

- [ ] **Step 7: Commit**

```bash
git add src/core/World.h src/core/World.cpp game_projects/sandcrawler/world.json tests/core/WorldTests.cpp tests/core/SandcrawlerWorldTests.cpp
git commit -m "Let a world declare its scene" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Scene retrieval

Automatic retrieval searches with the player's input, the scene values and the previous turn's digest, joined by newlines and cut to `EMBED_TEXT_CHARS` with the input first. A lore subject named in the input or in a scene value gets `NAME_BOOST` once, and each hit says why it matched. Assisted-mode lookups query with their own text only, as today.

**Files:**
- Create: `tests/SceneWorld.h`
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`, `src/turn/Session.cpp`
- Test: `tests/turn/RetrievalTests.cpp`, `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `core::World::scene`, `core::SceneDecl` (Task 7); `core::TurnRecord::digest` (Task 2); `ReviewScript`, `ReviewSettings` (Task 4).
- Produces:
  - `std::vector<std::string> turn::SceneOf(const core::World& world, const nlohmann::json& state)`.
  - `struct turn::RetrievalQuery { std::string input; std::vector<std::string> scene; std::string digest; std::string Text() const; }`.
  - `std::vector<RetrievalHit> turn::MemoryIndex::Retrieve(const RetrievalQuery& query, std::span<const store::EmbeddingKind> sources, int capTokens, std::size_t maxHits, inference::LlmBackend& backend) const`.
  - `std::vector<std::string> turn::RetrievalHit::reasons` (after `tokens`): `"name"`, `"scene"`, or empty.
  - Telemetry: each `timings["retrieval"]` entry gains `"reasons"`; `timings["scene"]` holds the scene values (an empty array for a world without a scene).
  - `std::filesystem::path test::SceneWorldDir(std::string_view name)` (`tests/SceneWorld.h`): a mini world copy at `FreshTempDir(name)/mini` with the scene `player.location` and lore for `shore` and `cliff top`.

- [ ] **Step 1: Add the scene test world**

Create `tests/SceneWorld.h`:

```cpp
#pragma once

#include <filesystem>
#include <string_view>

#include "TestPaths.h"

namespace morpheus::test {

// A copy of the mini test world in FreshTempDir(name)/mini (the folder name is the world id) that declares a scene,
// the player's location, and has lore for two places: "shore", where the player starts, and "cliff top". The rest of
// the lore is the mini world's. Returns the world folder.
inline std::filesystem::path SceneWorldDir(std::string_view name)
{
    const std::filesystem::path dir = FreshTempDir(name) / "mini";
    std::filesystem::copy(TestDataDir() / "worlds" / "mini", dir, std::filesystem::copy_options::recursive);
    WriteFile(dir / "world.json",
              R"({"id": "mini", "title": "Mini World", "dice": "1d20+stat", "check_entity": "player",)"
              R"( "stats": ["agility"], "difficulties": {"easy": 8, "hard": 16}, "memory_mode": "engine",)"
              R"( "scene": {"paths": ["player.location"], "present_at": "player.location"}})");
    WriteFile(dir / "lore.csv", "subject,data,always_on\n"
                                "lighthouse,\"A tall lighthouse, painted red.\",true\n"
                                "keeper,\"The keeper says \"\"hello\"\" to everyone.\",false\n"
                                "storm,Storms arrive at dusk.,\n"
                                "shore,Grey pebbles and driftwood line the shore.,\n"
                                "cliff top,Wind howls over the bare cliff top.,\n");
    return dir;
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing retrieval tests**

In `tests/turn/RetrievalTests.cpp`, add to the includes:

```cpp
#include <cstddef>

#include <nlohmann/json.hpp>
```

Every existing `index.Retrieve("...", ...)` call takes a `turn::RetrievalQuery` now:

```bash
sed -i 's/index\.Retrieve("\([^"]*\)", /index.Retrieve(turn::RetrievalQuery{"\1"}, /' tests/turn/RetrievalTests.cpp
grep -c 'Retrieve(turn::RetrievalQuery{' tests/turn/RetrievalTests.cpp
```

The count is 5.

In "A lore subject named in the input is boosted, and hits stay within the cap", after `CHECK(hits[0].refId == "keeper");` add:

```cpp
    CHECK(hits[0].reasons == std::vector<std::string>{"name"});
```

In "Only folded turns and notes are searchable history", inside the loop, after `CHECK(hit.refId != "2");` add:

```cpp
        CHECK(hit.reasons.empty()); // History and notes have no subject: similarity alone.
```

Inside the anonymous namespace, after `NewRun`, add:

```cpp
// The cosine of two texts under the scripted embedding, as Retrieve computes it.
float Similarity(test::ScriptedBackend& backend, const std::string& a, const std::string& b)
{
    const std::vector<std::vector<float>> vectors = backend.Embed({a, b}, inference::EmbedPurpose::Query);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    return similarity;
}

// The hit for lore `subject`, or nullptr.
const turn::RetrievalHit* FindLore(const std::vector<turn::RetrievalHit>& hits, const std::string& subject)
{
    for (const turn::RetrievalHit& hit : hits) {
        if (hit.kind == store::EmbeddingKind::Lore && hit.refId == subject) {
            return &hit;
        }
    }
    return nullptr;
}

// The sandcrawler world: its scene is the player's and the crawler's location, present at the player's.
core::World Sandcrawler()
{
    return core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");
}

// A sandcrawler NPC with `role`, at `location`.
nlohmann::json Npc(const std::string& role, const std::string& location)
{
    return {{"type", "npc"},
            {"fields",
             {{"role", role}, {"disposition", "wary"}, {"location", location},
              {"conditions", nlohmann::json::array()}, {"health", 10}}}};
}
```

Append:

```cpp
TEST_CASE("RetrievalQuery joins the input, the scene and the digest", "[retrieval]")
{
    CHECK(turn::RetrievalQuery{"I walk.", {"shore", "keeper"}, "The player rested."}.Text() ==
          "I walk.\nshore; keeper\nThe player rested.");
    CHECK(turn::RetrievalQuery{"I walk.", {}, ""}.Text() == "I walk.");
    CHECK(turn::RetrievalQuery{"I walk.", {}, "The player rested."}.Text() == "I walk.\nThe player rested.");

    // The input comes first, so it survives the cut to EMBED_TEXT_CHARS.
    const std::string longInput(turn::EMBED_TEXT_CHARS + 10, 'a');
    CHECK(turn::RetrievalQuery{longInput, {"shore"}, "x"}.Text() == longInput.substr(0, turn::EMBED_TEXT_CHARS));
}

TEST_CASE("A lore subject named by the scene is boosted once, with its reasons", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_scene", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);
    const std::string keeper = "keeper: The keeper says \"hello\" to everyone.";

    SECTION("named by the scene only")
    {
        const turn::RetrievalQuery query{"I look around.", {"near the keeper's door"}, ""};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons == std::vector<std::string>{"scene"});
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper) + turn::NAME_BOOST));
    }
    SECTION("named by the input and the scene: one boost, both reasons")
    {
        const turn::RetrievalQuery query{"I greet the keeper.", {"the keeper"}, ""};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons == std::vector<std::string>{"name", "scene"});
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper) + turn::NAME_BOOST));
    }
    SECTION("the digest only enters the embedding")
    {
        const turn::RetrievalQuery query{"I look around.", {}, "The keeper waved."};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons.empty());
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper)));
    }
    SECTION("scene hits are still trimmed to the cap")
    {
        const turn::RetrievalQuery query{"I look around.", {"keeper", "storm"}, ""};
        const int oneSnippet = backend.CountTokens("- storm: Storms arrive at dusk.\n");
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, oneSnippet, 6, backend);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].refId == "storm"); // The keeper's longer snippet does not fit.
    }
}

TEST_CASE("SceneOf lists the scene's values and who is present", "[retrieval]")
{
    const core::World world = Sandcrawler();
    nlohmann::json state = world.initialState; // The captain and the crawler are both in the "open desert".

    SECTION("the declared paths, then the entities at the player's location")
    {
        // The crawler shares the captain's location; it has no other string field.
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert", "crawler"});
    }
    SECTION("an NPC at the player's location adds its id and role, compared without case or spaces")
    {
        state["npc:marta"] = Npc("mechanic", "  Open Desert ");
        state["npc:raider"] = Npc("scout", "the ridge"); // Elsewhere: adds nothing.
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert", "crawler", "npc:marta", "mechanic"});
    }
    SECTION("duplicates collapse, keeping the first")
    {
        state["npc:marta"] = Npc("mechanic", "open desert");
        state["npc:otto"] = Npc("Mechanic", "open desert");
        CHECK(turn::SceneOf(world, state) ==
              std::vector<std::string>{"open desert", "crawler", "npc:marta", "mechanic", "npc:otto"});
    }
    SECTION("empty values are skipped")
    {
        state["crawler"]["fields"]["location"] = "";
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert"});
    }
    SECTION("a world without a scene has none")
    {
        CHECK(turn::SceneOf(MiniWorld(), MiniWorld().initialState).empty());
    }
}
```

- [ ] **Step 3: Write the failing session tests**

In `tests/turn/SessionTests.cpp`, add to the includes:

```cpp
#include <catch2/catch_approx.hpp>

#include "SceneWorld.h"
```

Append:

```cpp
TEST_CASE("Retrieval searches with the scene: moving the player changes the lore in the prompt", "[session][scene]")
{
    const std::string climb =
        R"({"intent":"climb","checks":[],"changes":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}]})";
    test::ScriptedBackend backend(16384, Responder(climb, "You look around."));
    turn::Session session = turn::Session::Start(core::LoadWorld(test::SceneWorldDir("session_scene_world")),
                                                 TestSettings(), NewRunFile("session_scene"), 42, backend);
    // A lore hit's reasons in a turn's telemetry; null when it did not reach the prompt.
    auto reasonsOf = [](const core::TurnRecord& record, const std::string& subject) {
        for (const nlohmann::json& hit : record.timings.at("retrieval")) {
            if (hit.at("source") == "lore" && hit.at("ref") == subject) {
                return hit.at("reasons");
            }
        }
        return nlohmann::json();
    };

    // On the shore the input names nothing, but the scene names the shore.
    const std::optional<core::TurnRecord> first = session.RunTurn("I look around.", {}, {});
    REQUIRE(first);
    CHECK(first->timings.at("scene") == nlohmann::json::array({"shore"}));
    CHECK(reasonsOf(*first, "shore") == nlohmann::json::array({"scene"}));
    CHECK_THAT(Tail(backend.Calls().at(0).prompt), ContainsSubstring("- shore: Grey pebbles"));

    // The first turn moved the player to the cliff top: the same input now brings the cliff top's lore.
    const std::optional<core::TurnRecord> second = session.RunTurn("I look around.", {}, {});
    REQUIRE(second);
    CHECK(second->timings.at("scene") == nlohmann::json::array({"cliff top"}));
    CHECK(reasonsOf(*second, "cliff top") == nlohmann::json::array({"scene"}));
    const nlohmann::json shore = reasonsOf(*second, "shore");
    CHECK((shore.is_null() || shore.empty())); // No longer named by the scene.
    CHECK_THAT(Tail(backend.Calls().at(2).prompt), ContainsSubstring("- cliff top: Wind howls"));
}

TEST_CASE("Retrieval searches with the previous turn's digest, without a boost", "[session][scene]")
{
    ReviewScript script{MOVE, {"You wait.", "You wait some more."},
                        {R"({"digest":"Storms arrive at dusk."})", R"({"digest":"The player waited."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_digest_query"), 42, backend);
    REQUIRE(session.RunTurn("I wait.", {}, {}));
    const std::optional<core::TurnRecord> second = session.RunTurn("I wait.", {}, {});
    REQUIRE(second);

    // The second query is the input, then the first turn's digest (the mini world declares no scene). The digest
    // names the storm, but only the input and the scene boost.
    const std::vector<std::vector<float>> vectors =
        backend.Embed({"I wait.\nStorms arrive at dusk.", "storm: Storms arrive at dusk."}, inference::EmbedPurpose::Query);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    const nlohmann::json* pStorm = nullptr;
    for (const nlohmann::json& hit : second->timings.at("retrieval")) {
        if (hit.at("ref") == "storm") {
            pStorm = &hit;
        }
    }
    REQUIRE(pStorm != nullptr);
    CHECK(pStorm->at("score").get<double>() == Catch::Approx(similarity));
    CHECK(pStorm->at("reasons").empty());
    CHECK(second->timings.at("scene") == nlohmann::json::array());
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `RetrievalQuery` and `SceneOf` are not members of `morpheus::turn`, and `reasons` is not a member of `RetrievalHit`.

- [ ] **Step 5: Add the query, the reasons and SceneOf**

In `src/turn/Retrieval.h`, add `#include <nlohmann/json.hpp>` to the includes. Replace the `NAME_BOOST` line with:

```cpp
constexpr float NAME_BOOST = 0.15f;                // Added once when a lore subject appears in the input or the scene.
```

Replace `RetrievalHit` with:

```cpp
// One retrieved snippet for the tail (T2).
struct RetrievalHit {
    store::EmbeddingKind kind = store::EmbeddingKind::Lore;
    std::string refId = {};
    std::string text = {}; // As the prompt shows it.
    float score = 0.0f;
    int tokens = 0; // What its prompt line costs against the retrieval cap (set by Retrieve).
    // Why it matched beyond similarity: "name" (its lore subject is in the input), "scene" (in a scene value). Empty
    // for similarity alone.
    std::vector<std::string> reasons = {};
};

// What retrieval searches with, in parts, so Retrieve can tell why an entry matched.
struct RetrievalQuery {
    std::string input = {};              // The player's input, or an assisted-mode lookup's query.
    std::vector<std::string> scene = {}; // SceneOf's values.
    std::string digest = {};             // The previous turn's digest.

    // What is embedded: the input, the scene values joined by "; ", then the digest, one per line (empty parts left
    // out), cut to EMBED_TEXT_CHARS bytes. The input comes first, so it survives the cut.
    std::string Text() const;
};

// The scene text for `state` (architecture spec, "World data"), empty for a world without a scene block: the value
// at each declared path, then every entity other than present_at's whose field of present_at's field name is a
// string equal to present_at's value (trimmed, lower-cased), in id order: its id, then its other string fields' values
// (in field name order). Empty values are skipped; values are deduplicated case-insensitively (trimmed), keeping the
// first as written.
std::vector<std::string> SceneOf(const core::World& world, const nlohmann::json& state);
```

Replace the `Retrieve` comment and declaration with:

```cpp
    // The best hits for `query` among `sources`. Each hit scores at least RETRIEVAL_MIN_SCORE: the cosine with the
    // embedding of query.Text(), plus NAME_BOOST, once, for a lore subject (lower-cased) contained in the input
    // (reason "name") or in a scene value (reason "scene"). The digest only enters the embedding. Hits come in score
    // order (ties by kind, then refId), at most maxHits of them, whose "- text\n" lines fit capTokens together. A hit
    // too big for the rest of the cap is skipped. No embedding call is made when nothing is searchable.
    std::vector<RetrievalHit> Retrieve(const RetrievalQuery& query, std::span<const store::EmbeddingKind> sources,
                                       int capTokens, std::size_t maxHits, inference::LlmBackend& backend) const;
```

In `src/turn/Retrieval.cpp`, after the anonymous namespace, add:

```cpp
std::string RetrievalQuery::Text() const
{
    // One part per line, in this order; empty parts are left out.
    std::string sceneText;
    for (const std::string& value : scene) {
        sceneText += (sceneText.empty() ? "" : "; ") + value;
    }
    const std::string* parts[] = {&input, &sceneText, &digest};
    std::string text;
    for (const std::string* pPart : parts) {
        if (!pPart->empty()) {
            text += (text.empty() ? "" : "\n") + *pPart;
        }
    }
    return core::CutUtf8(text, EMBED_TEXT_CHARS);
}

std::vector<std::string> SceneOf(const core::World& world, const nlohmann::json& state)
{
    if (!world.scene) {
        return {};
    }
    std::vector<std::string> values;
    std::set<std::string> seen; // Trimmed, lower-cased values kept so far.
    auto add = [&](const std::string& value) {
        const std::string key = Lowered(core::Trim(value));
        if (!key.empty() && seen.insert(key).second) {
            values.push_back(value);
        }
    };
    // A declared path's entity and field: "player.location" -> "player", "location". World loading checked the shape.
    auto split = [](const std::string& path) {
        const std::size_t dot = path.find('.');
        return std::pair<std::string, std::string>(path.substr(0, dot), path.substr(dot + 1));
    };
    // The string at `field` of an entity's fields, or "" when it has none.
    auto stringField = [](const nlohmann::json& entity, const std::string& field) {
        const nlohmann::json& fields = entity.at("fields");
        return fields.contains(field) && fields.at(field).is_string() ? fields.at(field).get<std::string>()
                                                                      : std::string();
    };

    // 1. The value at each declared path. Entities are never removed, so a scene entity is always in the state.
    for (const std::string& path : world.scene->paths) {
        const auto [entity, field] = split(path);
        MORPHEUS_REQUIRE(state.contains(entity), "SceneOf: a scene entity is missing from the state");
        add(stringField(state.at(entity), field));
    }

    // 2. Every other entity at present_at's value, by id (JSON objects iterate in key order): its id and its other
    // string fields. Nobody is present at an empty location.
    const auto [anchorEntity, anchorField] = split(world.scene->presentAt);
    MORPHEUS_REQUIRE(state.contains(anchorEntity), "SceneOf: the present_at entity is missing from the state");
    const std::string anchor = Lowered(core::Trim(stringField(state.at(anchorEntity), anchorField)));
    if (anchor.empty()) {
        return values;
    }
    for (auto it = state.begin(); it != state.end(); ++it) {
        if (it.key() == anchorEntity || Lowered(core::Trim(stringField(it.value(), anchorField))) != anchor) {
            continue;
        }
        add(it.key());
        const core::EntityTypeDef* pType = world.schema.FindEntityType(it.value().at("type").get<std::string>());
        MORPHEUS_REQUIRE(pType != nullptr, "SceneOf: the state has an unknown entity type");
        for (const auto& [name, def] : pType->fields) {
            if (def.type == core::FieldType::String && name != anchorField) {
                add(stringField(it.value(), name));
            }
        }
    }
    return values;
}
```

Replace `MemoryIndex::Retrieve` with:

```cpp
std::vector<RetrievalHit> MemoryIndex::Retrieve(const RetrievalQuery& query,
                                                std::span<const store::EmbeddingKind> sources, int capTokens,
                                                std::size_t maxHits, inference::LlmBackend& backend) const
{
    auto searched = [&sources](store::EmbeddingKind kind) {
        return std::find(sources.begin(), sources.end(), kind) != sources.end();
    };
    const std::string text = query.Text();
    const bool anything = std::any_of(_entries.begin(), _entries.end(), [&](const Entry& e) { return searched(e.kind); });
    if (!anything || capTokens <= 0 || maxHits == 0 || text.empty()) {
        return {};
    }

    // The whole query is embedded; the input and the scene values are also matched against lore subjects.
    const std::vector<float> queryVector = backend.Embed({text}, inference::EmbedPurpose::Query).at(0);
    const std::string loweredInput = Lowered(query.input);
    std::vector<std::string> loweredScene;
    for (const std::string& value : query.scene) {
        loweredScene.push_back(Lowered(value));
    }
    std::vector<RetrievalHit> hits;
    for (const Entry& entry : _entries) {
        if (!searched(entry.kind)) {
            continue;
        }
        MORPHEUS_REQUIRE(entry.vector.size() == queryVector.size(), "Retrieve: vectors from different models");
        float score = 0.0f;
        for (std::size_t i = 0; i < queryVector.size(); ++i) {
            score += queryVector[i] * entry.vector[i]; // Both are L2-normalized: the dot product is the cosine.
        }
        // A lore subject named in the input, in a scene value or in both is boosted once.
        std::vector<std::string> reasons;
        if (!entry.name.empty()) {
            if (loweredInput.find(entry.name) != std::string::npos) {
                reasons.push_back("name");
            }
            const bool inScene = std::any_of(loweredScene.begin(), loweredScene.end(), [&entry](const std::string& value) {
                return value.find(entry.name) != std::string::npos;
            });
            if (inScene) {
                reasons.push_back("scene");
            }
        }
        if (!reasons.empty()) {
            score += NAME_BOOST;
        }
        if (score >= RETRIEVAL_MIN_SCORE) {
            hits.push_back({entry.kind, entry.refId, entry.text, score, 0, std::move(reasons)});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RetrievalHit& a, const RetrievalHit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.refId < b.refId;
    });

    std::vector<RetrievalHit> kept;
    int used = 0;
    for (RetrievalHit& hit : hits) {
        if (kept.size() == maxHits) {
            break;
        }
        const int cost = backend.CountTokens("- " + hit.text + "\n");
        if (used + cost > capTokens) {
            continue;
        }
        used += cost;
        hit.tokens = cost;
        kept.push_back(std::move(hit));
    }
    return kept;
}
```

- [ ] **Step 6: Query with the scene and the previous digest**

In `src/turn/Session.cpp`, replace

```cpp
    // 1. Retrieve. The query is cut like embedded documents, so a very long input still fits the embedding context.
    std::vector<RetrievalHit> hits = _index.Retrieve(core::CutUtf8(input, EMBED_TEXT_CHARS), ALL_SOURCES,
                                                     _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);
```

with

```cpp
    // 1. Retrieve, with the input, the scene and the previous turn's digest. RetrievalQuery::Text cuts the query like
    // embedded documents, input first, so a very long input still fits the embedding context.
    RetrievalQuery query;
    query.input = input;
    query.scene = SceneOf(_world, state);
    if (turn > 1) {
        const std::optional<core::TurnRecord> previous = _store.Turn(turn - 1);
        MORPHEUS_REQUIRE(previous.has_value(), "PlayTurn: the previous turn is not in the store");
        query.digest = previous->digest;
    }
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, ALL_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);
```

In the lookup loop, replace

```cpp
            // The query is cut like the automatic one above.
            for (RetrievalHit& hit : _index.Retrieve(core::CutUtf8(lookup.query, EMBED_TEXT_CHARS), source, capLeft,
                                                     LOOKUP_MAX_HITS, *_pBackend)) {
```

with

```cpp
            // A lookup searches with its own query only (RetrievalQuery::Text cuts it like the automatic one).
            for (RetrievalHit& hit :
                 _index.Retrieve(RetrievalQuery{lookup.query}, source, capLeft, LOOKUP_MAX_HITS, *_pBackend)) {
```

In the commit step, replace

```cpp
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back({{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}});
    }
```

with

```cpp
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back(
            {{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}, {"reasons", hit.reasons}});
    }
    telemetry["scene"] = query.scene; // The scene values the automatic query searched with.
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[retrieval],[session]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass. The replays' queries now carry digests (and the sandcrawler scene), which changes only which hits reach the prompts, not the committed turns the replay checks.

- [ ] **Step 8: Commit**

```bash
git add tests/SceneWorld.h src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/Session.cpp tests/turn/RetrievalTests.cpp tests/turn/SessionTests.cpp
git commit -m "Retrieve with the scene and the previous turn's digest" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
### Task 9: The app's Settings, Turn log and Memory windows

The Settings window edits `narration_check`, `compact_transcript`, `prose_turns` and `review_thinking_tokens` (beside the other thinking budgets), and lists `review_max_tokens` with the read-only budget. The Turn log shows each review's verdict, problems and digest, the renarrate's call, and the narration it replaced. The Memory window shows each hit's reasons next to its score, and the latest turn's scene values. (The Play window already shows only the new narration after a restart: `RunModel` clears it, Task 5.)

**Files:**
- Modify: `src/app/model/SettingsEdit.h`, `src/app/model/SettingsEdit.cpp`, `src/app/model/Inspect.h`, `src/app/model/Inspect.cpp`, `src/app/ui/Windows.h`, `src/app/ui/SettingsWindow.cpp`, `src/app/ui/TurnLogWindow.cpp`, `src/app/ui/MemoryWindow.cpp`
- Test: `tests/app/SettingsEditTests.cpp`, `tests/app/InspectTests.cpp`, `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: the settings keys (Task 1); the telemetry `review`, `renarrate`, `retrieval[].reasons`, `scene` (Tasks 4 and 8); `turn::REVIEW_RULES`, `turn::RENARRATE_CORRECTION` (Task 3); `test::SceneWorldDir` (Task 8).
- Produces:
  - `app::SettingsEdit::reviewThinkingTokens` (`int`), `narrationCheck` (`bool`), `compactTranscript` (`bool`), `proseTurns` (`int`).
  - `std::vector<std::string> app::RetrievalRow::reasons`.
  - `struct app::ReviewRow { CallStats call; std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`.
  - `std::vector<app::ReviewRow> app::ReviewRows(const nlohmann::json& timings)`, `std::optional<std::string> app::ReplacedNarration(const nlohmann::json& timings)`, `std::vector<std::string> app::SceneValues(const nlohmann::json& timings)`.

- [ ] **Step 1: Write the failing settings edit tests**

In `tests/app/SettingsEditTests.cpp`, in "ReadSettingsEdit reads the edited values as the file writes them", after the `narrateThinkingTokens` check add:

```cpp
    CHECK(edit.reviewThinkingTokens == file.at("budget").at("review_thinking_tokens").get<int>());
    CHECK(edit.narrationCheck == file.at("narration_check").get<bool>());
    CHECK(edit.compactTranscript == file.at("compact_transcript").get<bool>());
    CHECK(edit.proseTurns == file.at("prose_turns").get<int>());
```

Append:

```cpp
TEST_CASE("SaveSettingsEdit writes the review settings and refuses prose_turns 0", "[settings_edit]")
{
    const std::filesystem::path dir = test::SettingsFolder("settings_edit_review");
    const std::filesystem::path file = dir / "settings.json";
    app::SettingsEdit edit = app::ReadSettingsEdit(file);
    edit.narrationCheck = false;
    edit.compactTranscript = false;
    edit.proseTurns = 3;
    edit.reviewThinkingTokens = 128;

    const core::Settings saved = app::SaveSettingsEdit(file, edit);
    CHECK_FALSE(saved.narrationCheck);
    CHECK_FALSE(saved.compactTranscript);
    CHECK(saved.proseTurns == 3);
    CHECK(saved.budget.reviewThinkingTokens == 128);
    CHECK(saved.budget.reviewMaxTokens == 384); // Not edited: kept.

    // The loader's rule reaches the player, and the file stays as it was.
    const std::string before = test::ReadFile(file);
    edit.proseTurns = 0;
    CHECK_THROWS_WITH(app::SaveSettingsEdit(file, edit), ContainsSubstring("prose_turns"));
    CHECK(test::ReadFile(file) == before);
}
```

- [ ] **Step 2: Write the failing inspector tests**

In `tests/app/InspectTests.cpp`, in "RetrievalRows shows each hit's text", replace the `timings` initializer with:

```cpp
    const nlohmann::json timings = nlohmann::json::parse(R"({"retrieval": [
        {"source": "lore", "ref": "keeper", "score": 0.8, "reasons": ["name", "scene"]},
        {"source": "history", "ref": "1", "score": 0.6, "reasons": []},
        {"source": "notes", "ref": "1.1", "score": 0.5, "reasons": []}]})");
```

after `CHECK(rows[0].score == 0.8);` add:

```cpp
    CHECK(rows[0].reasons == std::vector<std::string>{"name", "scene"});
    CHECK(rows[1].reasons.empty());
```

and replace the `gone` initializer with:

```cpp
    const nlohmann::json gone =
        nlohmann::json::parse(R"({"retrieval": [{"source": "history", "ref": "7", "score": 0.5, "reasons": []}]})");
```

Append:

```cpp
TEST_CASE("ReviewRows and ReplacedNarration read a renarrated turn's reviews", "[inspect]")
{
    const nlohmann::json timings = nlohmann::json::parse(R"({
        "review": [
            {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 40, "prompt_ms": 5.0,
             "generate_ms": 300.0, "stopped_at_limit": false,
             "consistent": false, "problems": ["The climb failed."], "digest": "The player reached the top."},
            {"prompt_tokens": 410, "reused": 0, "evaluated": 410, "generated": 30, "prompt_ms": 5.0,
             "generate_ms": 250.0, "stopped_at_limit": false, "consistent": true, "digest": "The player slipped.",
             "thinking": {"tokens": 70, "ms": 900.0, "stopped_at_limit": false, "text": "Check the roll."}}],
        "renarrate": {"prompt_tokens": 1200, "reused": 1100, "evaluated": 100, "generated": 90, "prompt_ms": 20.0,
                      "generate_ms": 1500.0, "stopped_at_limit": false, "replaced": "You reach the top."}})");

    const std::vector<app::ReviewRow> rows = app::ReviewRows(timings);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].call.promptTokens == 400);
    CHECK(rows[0].consistent == false);
    CHECK(rows[0].problems == std::vector<std::string>{"The climb failed."});
    CHECK(rows[0].digest == "The player reached the top.");
    CHECK_FALSE(rows[0].call.thinking);
    CHECK(rows[1].consistent == true);
    CHECK(rows[1].problems.empty());
    REQUIRE(rows[1].call.thinking);
    CHECK(rows[1].call.thinking->text == "Check the roll.");
    CHECK(app::ReplacedNarration(timings) == "You reach the top.");
    const std::optional<app::CallStats> renarrate = app::CallStatsOf(timings, "renarrate");
    REQUIRE(renarrate);
    CHECK(renarrate->generated == 90);

    // An unreadable review keeps only its reason; a turn without reviews has none.
    const std::vector<app::ReviewRow> failed = app::ReviewRows(nlohmann::json::parse(R"({"review": [
        {"prompt_tokens": 400, "reused": 0, "evaluated": 400, "generated": 384, "prompt_ms": 5.0,
         "generate_ms": 3000.0, "stopped_at_limit": true, "error": "the review output is not complete JSON"}]})"));
    REQUIRE(failed.size() == 1);
    CHECK_FALSE(failed[0].consistent);
    CHECK(failed[0].call.stoppedAtLimit);
    CHECK(failed[0].error == "the review output is not complete JSON");
    CHECK(app::ReviewRows(nlohmann::json::object()).empty());
    CHECK_FALSE(app::ReplacedNarration(nlohmann::json::object()));
    CHECK_THROWS_AS(app::ReviewRows(nlohmann::json::parse(R"({"review": [{"consistent": true}]})")), core::DataError);
}

TEST_CASE("SceneValues reads the scene a turn's retrieval searched with", "[inspect]")
{
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": ["shore", "npc:marta"]})")) ==
          std::vector<std::string>{"shore", "npc:marta"});
    CHECK(app::SceneValues(nlohmann::json::parse(R"({"scene": []})")).empty());
    CHECK(app::SceneValues(nlohmann::json::object()).empty());
    CHECK_THROWS_AS(app::SceneValues(nlohmann::json::parse(R"({"scene": "shore"})")), core::DataError);
}
```

- [ ] **Step 3: Write the failing UI test**

In `tests/app/UiTests.cpp`, add to the includes:

```cpp
#include <algorithm>
#include <vector>

#include "SceneWorld.h"
#include "app/model/Inspect.h"
#include "turn/EngineText.h"
```

Inside the anonymous namespace, after `AssistedResponder`, add:

```cpp
const std::string RENARRATION = "You walk the last steps along the shore to the lighthouse door.";

// Resolve calls get MOVE. Each turn's first review finds a contradiction and its second none (the count is shared
// by every backend the factory makes). The first narration is NARRATION, the renarrate (its message carries the
// correction) RENARRATION.
test::ScriptedBackend::Responder RenarrateResponder()
{
    auto pReviews = std::make_shared<int>(0);
    return [pReviews](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("intent") != std::string::npos) {
            return MOVE;
        }
        if (prompt.messages.front().content == turn::REVIEW_RULES) {
            return (*pReviews)++ % 2 == 0
                       ? R"({"consistent":false,"problems":["The player never reaches the lighthouse."],"digest":"The player set off."})"
                       : R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";
        }
        const bool renarrate = prompt.messages.back().content.find(turn::RENARRATE_CORRECTION) != std::string::npos;
        return renarrate ? RENARRATION : NARRATION;
    };
}
```

Append:

```cpp
TEST_CASE("The inspectors draw a renarrated turn and scene retrieval", "[ui]")
{
    test::HeadlessImGui imgui;
    // settings.example.json turns the narration check and the compact transcript on. The only world is the mini
    // world with a scene, whose player starts on the shore.
    const std::filesystem::path dir = test::SettingsFolder("ui_review");
    app::AppPaths paths = test::PathsIn(dir);
    paths.projectsDir = test::SceneWorldDir("ui_review_worlds").parent_path();
    app::AppController controller(paths, test::ScriptedFactory(RenarrateResponder()));
    app::Ui ui;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "review", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    controller.Submit("I look around.");
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    REQUIRE(controller.Run().History().size() == 1);
    const core::TurnRecord& turn = controller.Run().History().back();
    CHECK(turn.narration == RENARRATION);
    CHECK(app::ReplacedNarration(turn.timings) == NARRATION);
    CHECK(app::ReviewRows(turn.timings).size() == 2);
    CHECK(app::SceneValues(turn.timings) == std::vector<std::string>{"shore"});
    const std::vector<app::RetrievalRow> rows =
        app::RetrievalRows(turn.timings, controller.Run().History(), controller.Run().Memory().notes);
    const bool sceneHit = std::any_of(rows.begin(), rows.end(), [](const app::RetrievalRow& row) {
        return row.ref == "shore" && std::find(row.reasons.begin(), row.reasons.end(), "scene") != row.reasons.end();
    });
    CHECK(sceneHit);
    DrawFrames(imgui, controller, ui); // The Turn log with both reviews and the replaced narration; the Memory window
                                       // with the reasons and the scene; the Settings window with the new keys.
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `reviewThinkingTokens`, `narrationCheck`, `compactTranscript` and `proseTurns` are not members of `SettingsEdit`; `reasons` is not a member of `RetrievalRow`; `ReviewRows`, `ReviewRow`, `ReplacedNarration` and `SceneValues` are not members of `morpheus::app`.

- [ ] **Step 5: Read and save the new keys**

In `src/app/model/SettingsEdit.h`, replace the struct with:

```cpp
// The settings.json values the Settings window edits. Model paths stay as written in the file (often relative to
// its folder), so saving does not rewrite them as absolute paths.
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    // The budget's thinking blocks, edited beside the model because they depend on it: a model without a thinking
    // mode needs all three at 0.
    int resolveThinkingTokens = 0;
    int narrateThinkingTokens = 0;
    int reviewThinkingTokens = 0;
    core::SamplingSettings resolveSampling = {};
    core::SamplingSettings narrateSampling = {};
    std::optional<core::MemoryMode> memoryMode = {}; // nullopt: each world's own default.
    bool narrationCheck = false;    // narration_check
    bool compactTranscript = false; // compact_transcript
    int proseTurns = 0;             // prose_turns
};
```

and in the `SaveSettingsEdit` comment replace `(the embedding prefixes, the budget)` with `(the embedding prefixes, the rest of the budget)`.

In `src/app/model/SettingsEdit.cpp`, in `ReadSettingsEdit` after the `narrateThinkingTokens` line add:

```cpp
        edit.reviewThinkingTokens = json.at("budget").at("review_thinking_tokens").get<int>();
```

and after the `narrateSampling` line add:

```cpp
        edit.narrationCheck = json.at("narration_check").get<bool>();
        edit.compactTranscript = json.at("compact_transcript").get<bool>();
        edit.proseTurns = json.at("prose_turns").get<int>();
```

In `SaveSettingsEdit` after the `narrate_thinking_tokens` line add:

```cpp
        json["budget"]["review_thinking_tokens"] = edit.reviewThinkingTokens;
```

and after the `json["sampling"]["narrate"]` line add:

```cpp
        json["narration_check"] = edit.narrationCheck;
        json["compact_transcript"] = edit.compactTranscript;
        json["prose_turns"] = edit.proseTurns;
```

- [ ] **Step 6: Read the reviews, reasons and scene from the telemetry**

In `src/app/model/Inspect.h`, replace `RetrievalRow` with:

```cpp
// One retrieval hit that reached the turn's resolve prompt.
struct RetrievalRow {
    std::string source = {}; // "lore", "history" or "notes"
    std::string ref = {};
    double score = 0.0;
    std::vector<std::string> reasons = {}; // "name", "scene"; empty for similarity alone.
    std::string text = {};                 // The lore subject, the turn's input, or the note.
};

// One review call of a turn, from its telemetry.
struct ReviewRow {
    CallStats call = {};
    std::optional<bool> consistent = {}; // Set when the narration check was on and the output was readable.
    std::vector<std::string> problems = {};
    std::string digest = {};
    std::string error = {}; // Why the review output could not be read.
};
```

Replace the `CallStatsOf` comment with:

```cpp
// The numbers of call `call` ("lookup", "resolve", "narrate" or "renarrate") in a turn's telemetry, with its thinking
// block when it had one; nullopt when the turn made no such call.
```

After the `RetrievalRows` declaration add:

```cpp
// A turn's review calls, in the order made: one, or two when the narration was renarrated. Empty when the turn made
// none. Throws DataError when an entry is malformed.
std::vector<ReviewRow> ReviewRows(const nlohmann::json& timings);

// The first narration of a renarrated turn, the one the second replaced; nullopt when the turn was not renarrated.
// Throws DataError when the renarrate entry has no replaced narration.
std::optional<std::string> ReplacedNarration(const nlohmann::json& timings);

// The scene values automatic retrieval searched with; empty when there were none. Throws DataError when the entry is
// not a list of strings.
std::vector<std::string> SceneValues(const nlohmann::json& timings);
```

In `src/app/model/Inspect.cpp`, add an anonymous namespace after the namespace opening:

```cpp
namespace {

// One call's numbers from its telemetry object, with its thinking block when it had one. Throws nlohmann's
// exceptions for a malformed object; the callers say which entry it was.
CallStats ReadCallStats(const nlohmann::json& numbers)
{
    CallStats stats{numbers.at("prompt_tokens").get<int>(), numbers.at("reused").get<int>(),
                    numbers.at("evaluated").get<int>(),     numbers.at("generated").get<int>(),
                    numbers.at("prompt_ms").get<double>(),  numbers.at("generate_ms").get<double>(),
                    numbers.at("stopped_at_limit").get<bool>()};
    // Only calls that were allowed to think carry a thinking block.
    if (numbers.contains("thinking")) {
        const nlohmann::json& thinking = numbers.at("thinking");
        stats.thinking = ThinkingStats{thinking.at("tokens").get<int>(), thinking.at("ms").get<double>(),
                                       thinking.at("stopped_at_limit").get<bool>(),
                                       thinking.at("text").get<std::string>()};
    }
    return stats;
}

} // namespace
```

Replace `CallStatsOf` with:

```cpp
std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call)
{
    const std::string key(call);
    if (!timings.is_object() || !timings.contains(key)) {
        return std::nullopt;
    }
    try {
        return ReadCallStats(timings.at(key));
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry '{}': {}", key, error.what()));
    }
}
```

In `RetrievalRows`, after `row.score = hit.at("score").get<double>();` add:

```cpp
            row.reasons = hit.at("reasons").get<std::vector<std::string>>();
```

After `RetrievalRows`, add:

```cpp
std::vector<ReviewRow> ReviewRows(const nlohmann::json& timings)
{
    std::vector<ReviewRow> rows;
    if (!timings.is_object() || !timings.contains("review")) {
        return rows;
    }
    try {
        // Each entry is the call's numbers plus whatever the review said: its verdict and problems with the check on,
        // its digest with the compact transcript on, or why its output could not be read.
        for (const nlohmann::json& entry : timings.at("review")) {
            ReviewRow row;
            row.call = ReadCallStats(entry);
            if (entry.contains("consistent")) {
                row.consistent = entry.at("consistent").get<bool>();
            }
            row.problems = entry.value("problems", std::vector<std::string>());
            row.digest = entry.value("digest", std::string());
            row.error = entry.value("error", std::string());
            rows.push_back(std::move(row));
        }
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'review': {}", error.what()));
    }
    return rows;
}

std::optional<std::string> ReplacedNarration(const nlohmann::json& timings)
{
    if (!timings.is_object() || !timings.contains("renarrate")) {
        return std::nullopt;
    }
    const nlohmann::json& renarrate = timings.at("renarrate");
    if (!renarrate.is_object() || !renarrate.contains("replaced") || !renarrate.at("replaced").is_string()) {
        throw core::DataError("turn telemetry 'renarrate' has no 'replaced' narration");
    }
    return renarrate.at("replaced").get<std::string>();
}

std::vector<std::string> SceneValues(const nlohmann::json& timings)
{
    if (!timings.is_object() || !timings.contains("scene")) {
        return {};
    }
    try {
        return timings.at("scene").get<std::vector<std::string>>();
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'scene': {}", error.what()));
    }
}
```

Add `#include <utility>` to the includes of `src/app/model/Inspect.cpp`.

- [ ] **Step 7: Edit the new keys in the Settings window**

In `src/app/ui/SettingsWindow.cpp`, after

```cpp
            ImGui::InputInt("Narrate thinking tokens", &edit.narrateThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
```

add

```cpp
            ImGui::InputInt("Review thinking tokens", &edit.reviewThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
```

After the memory mode combo's closing brace (the `if (ImGui::Combo("Memory mode", ...)) { ... }` block), add:

```cpp
            // The narration review, and the compact transcript built from its digests.
            ImGui::SeparatorText("Narration review");
            ImGui::Checkbox("Narration check", &edit.narrationCheck);
            ImGui::SetItemTooltip("After each narration a review call checks it against the outcome; a narration "
                                  "that contradicts it is written once more.");
            ImGui::Checkbox("Compact transcript", &edit.compactTranscript);
            ImGui::SetItemTooltip("The review also writes a short digest of each turn, and older turns reach the "
                                  "prompt as their digest.");
            ImGui::BeginDisabled(!edit.compactTranscript);
            ImGui::InputInt("Prose turns", &edit.proseTurns);
            ImGui::SetItemTooltip("The newest turns that keep their full narration in the prompt. At least 1.");
            ImGui::EndDisabled();
```

Replace the budget `rows` array with:

```cpp
            const std::pair<const char*, int> rows[] = {
                {"resolve_max_tokens", budget.resolveMaxTokens}, {"narrate_max_tokens", budget.narrateMaxTokens},
                {"review_max_tokens", budget.reviewMaxTokens},   {"narrate_allowance", budget.narrateAllowance},
                {"tail_allowance", budget.tailAllowance},        {"engine_rules_cap", budget.engineRulesCap},
                {"world_cap", budget.worldCap},                  {"summary_cap", budget.summaryCap},
                {"retrieval_cap", budget.retrievalCap}};
```

- [ ] **Step 8: Show the reviews in the Turn log**

In `src/app/ui/Windows.h`, replace the `TurnLogWindow` and `MemoryWindow` comments with:

```cpp
// Every turn, newest first: input, intent, rolls, applied and rejected changes, notes, lookups, the narration
// review (verdicts, problems, digests and a replaced narration), per-call numbers and the raw resolve output.
```

```cpp
// The story summaries, the notes, and the latest turn's retrieval hits with their scores and reasons, and its scene.
```

Replace `src/app/ui/TurnLogWindow.cpp` with:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "app/model/Inspect.h"
#include "context/Transcript.h"
#include "core/Text.h"

namespace morpheus::app {

namespace {

constexpr std::size_t TITLE_INPUT_CHARS = 60; // The input shown in a turn's header, in bytes.
constexpr float RESOLVE_JSON_LINES = 12.0f;   // Height of the raw resolve output, in lines.
constexpr float THINKING_LINES = 16.0f;       // Height of a call's thinking text, in lines.
constexpr float REPLACED_LINES = 8.0f;        // Height of a replaced narration, in lines.
// Model calls with one telemetry entry each, in turn order. The reviews (a list) follow them.
constexpr const char* CALLS[] = {"lookup", "resolve", "narrate", "renarrate"};

} // namespace

void TurnLogWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(TURN_LOG_WINDOW)) {
        const std::vector<core::TurnRecord>& history = controller.Run().History();
        if (history.empty()) {
            ImGui::TextDisabled("No turns yet.");
        }
        // Newest first; the latest starts open.
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const core::TurnRecord& turn = *it;
            ImGui::PushID(turn.number);
            if (it == history.rbegin()) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            // "###header" keeps the header's ID stable while its label changes (a regenerate adds an attempt).
            const std::string attempt = turn.attempt > 0 ? std::format(" (attempt {})", turn.attempt + 1) : std::string();
            const std::string title = std::format("Turn {}{}: {}###header", turn.number, attempt,
                                                  core::CutUtf8(turn.input, TITLE_INPUT_CHARS));
            if (ImGui::CollapsingHeader(title.c_str())) {
                // What the player asked and what the model made of it.
                ImGui::TextWrapped("Input: %s", turn.input.c_str());
                const nlohmann::json intent =
                    turn.resolve.is_object() ? turn.resolve.value("intent", nlohmann::json()) : nlohmann::json();
                ImGui::TextWrapped("Intent: %s", intent.is_string() ? intent.get<std::string>().c_str() : "(none)");

                if (!turn.checks.empty()) {
                    ImGui::SeparatorText("Checks");
                    for (const core::CheckOutcome& check : turn.checks) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s (stakes: %s)", CheckText(check).c_str(), check.stakes.c_str());
                    }
                }
                if (!turn.applied.empty()) {
                    ImGui::SeparatorText("Applied");
                    for (const core::AppliedChange& applied : turn.applied) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s - %s", context::RenderChangePart(applied).c_str(),
                                           applied.change.reason.c_str());
                    }
                }
                if (!turn.rejected.empty()) {
                    ImGui::SeparatorText("Rejected");
                    for (const core::RejectedChange& rejected : turn.rejected) {
                        TextWrappedColored(ERROR_COLOR, std::format("{} - {}", rejected.raw.dump(), rejected.reason));
                    }
                }
                if (!turn.notes.empty()) {
                    ImGui::SeparatorText("Notes");
                    for (const std::string& note : turn.notes) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s", note.c_str());
                    }
                }

                // Assisted mode: what the lookup call asked for, and how much of it reached the prompt.
                const nlohmann::json& timings = turn.timings;
                if (timings.is_object() && timings.contains("lookup")) {
                    ImGui::SeparatorText("Lookups");
                    const nlohmann::json& lookup = timings.at("lookup");
                    for (const nlohmann::json& request : lookup.value("requests", nlohmann::json::array())) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s \"%s\": %zu found, %zu reached the prompt",
                                           request.at("source").get<std::string>().c_str(),
                                           request.at("query").get<std::string>().c_str(), request.at("added").size(),
                                           request.value("kept", nlohmann::json::array()).size());
                    }
                    if (lookup.contains("error")) {
                        TextWrappedColored(ERROR_COLOR,
                                           "Unreadable lookup output: " + lookup.at("error").get<std::string>());
                    }
                }

                // The narration review: each review's verdict, its problems and digest, and the narration a renarrate
                // replaced.
                const std::vector<ReviewRow> reviews = ReviewRows(timings);
                if (!reviews.empty()) {
                    ImGui::SeparatorText("Review");
                    for (std::size_t i = 0; i < reviews.size(); ++i) {
                        const ReviewRow& review = reviews[i];
                        const char* verdict = !review.consistent ? "no verdict"
                                              : *review.consistent ? "consistent with the outcome"
                                                                   : "contradicts the outcome";
                        const std::string line = std::format("Review {}: {}", i + 1, verdict);
                        if (review.consistent == false) {
                            TextWrappedColored(WARNING_COLOR, line);
                        } else {
                            ImGui::TextWrapped("%s", line.c_str());
                        }
                        for (const std::string& problem : review.problems) {
                            ImGui::Bullet();
                            ImGui::TextWrapped("%s", problem.c_str());
                        }
                        if (!review.digest.empty()) {
                            ImGui::TextWrapped("Digest: %s", review.digest.c_str());
                        }
                        if (!review.error.empty()) {
                            TextWrappedColored(ERROR_COLOR, "Unreadable review output: " + review.error);
                        }
                    }
                    if (const std::optional<std::string> replaced = ReplacedNarration(timings);
                        replaced && ImGui::TreeNode("Replaced narration")) {
                        std::string text = *replaced;
                        ImGui::InputTextMultiline("##replaced", &text,
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * REPLACED_LINES),
                                                  ImGuiInputTextFlags_ReadOnly);
                        ImGui::TreePop();
                    }
                }

                // Each model call's numbers: one row per call, the reviews numbered.
                ImGui::SeparatorText("Calls");
                auto callRow = [](const std::string& label, const CallStats& stats) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(label.c_str());
                    if (stats.stoppedAtLimit) {
                        ImGui::SameLine();
                        ImGui::TextColored(WARNING_COLOR, "(hit max tokens)");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.promptTokens);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.reused);
                    // A call that did not think shows dashes, not zeros.
                    ImGui::TableNextColumn();
                    if (stats.thinking) {
                        ImGui::Text("%d", stats.thinking->tokens);
                        if (stats.thinking->stoppedAtLimit) {
                            ImGui::SameLine();
                            ImGui::TextColored(WARNING_COLOR, "(hit budget)");
                        }
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    if (stats.thinking) {
                        ImGui::Text("%.0f", stats.thinking->ms);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats.generated);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", stats.promptMs);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", stats.generateMs);
                };
                if (ImGui::BeginTable("calls", 8,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const char* header :
                         {"Call", "Prompt", "Reused", "Thinking", "Thinking ms", "Generated", "Prompt ms", "Generate ms"}) {
                        ImGui::TableSetupColumn(header);
                    }
                    ImGui::TableHeadersRow();
                    for (const char* call : CALLS) {
                        if (const std::optional<CallStats> stats = CallStatsOf(timings, call)) {
                            callRow(call, *stats);
                        }
                    }
                    for (std::size_t i = 0; i < reviews.size(); ++i) {
                        callRow(std::format("review {}", i + 1), reviews[i].call);
                    }
                    ImGui::EndTable();
                }
                if (timings.is_object() && timings.contains("resolve") && timings.at("resolve").contains("error")) {
                    TextWrappedColored(ERROR_COLOR, "Unreadable resolve output: " +
                                                        timings.at("resolve").at("error").get<std::string>());
                }

                // Each call's reasoning, as the model wrote it in its thinking block.
                auto thinkingNode = [](const std::string& label, const CallStats& stats) {
                    if (!stats.thinking) {
                        return;
                    }
                    if (ImGui::TreeNode(label.c_str(), "%s thinking", label.c_str())) {
                        std::string text = stats.thinking->text;
                        ImGui::InputTextMultiline("##thinking", &text,
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * THINKING_LINES),
                                                  ImGuiInputTextFlags_ReadOnly);
                        ImGui::TreePop();
                    }
                };
                for (const char* call : CALLS) {
                    if (const std::optional<CallStats> stats = CallStatsOf(timings, call)) {
                        thinkingNode(call, *stats);
                    }
                }
                for (std::size_t i = 0; i < reviews.size(); ++i) {
                    thinkingNode(std::format("review {}", i + 1), reviews[i].call);
                }

                // The resolve output exactly as the model wrote it.
                if (ImGui::TreeNode("Resolve output (JSON)")) {
                    std::string json = turn.resolve.dump(2);
                    ImGui::InputTextMultiline("##resolve", &json,
                                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * RESOLVE_JSON_LINES),
                                              ImGuiInputTextFlags_ReadOnly);
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 9: Show the reasons and the scene in the Memory window**

In `src/app/ui/MemoryWindow.cpp`, replace everything from `            // What retrieval put in the latest turn's prompt.` through the table's `ImGui::EndTable();` and the closing brace of that `else if` with:

```cpp
            // What retrieval put in the latest turn's prompt, why each hit matched, and the scene it searched with.
            ImGui::SeparatorText("Retrieved for the latest turn");
            const core::TurnRecord* pLatest = run.LatestTurn();
            // Values joined with `separator`, for the scene line and a hit's reasons.
            auto joined = [](const std::vector<std::string>& values, const char* separator) {
                std::string text;
                for (const std::string& value : values) {
                    text += (text.empty() ? "" : separator) + value;
                }
                return text;
            };
            const std::vector<std::string> scene =
                pLatest ? SceneValues(pLatest->timings) : std::vector<std::string>();
            if (!scene.empty()) {
                ImGui::TextWrapped("Scene: %s", joined(scene, "; ").c_str());
            }
            const std::vector<RetrievalRow> rows =
                pLatest ? RetrievalRows(pLatest->timings, run.History(), memory.notes) : std::vector<RetrievalRow>();
            if (rows.empty()) {
                ImGui::TextDisabled("Nothing retrieved.");
            } else if (ImGui::BeginTable("hits", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Reasons", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const RetrievalRow& row : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.source.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.ref.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", row.score);
                    // Similarity alone shows a dash.
                    ImGui::TableNextColumn();
                    if (row.reasons.empty()) {
                        ImGui::TextDisabled("-");
                    } else {
                        ImGui::TextUnformatted(joined(row.reasons, ", ").c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", row.text.c_str());
                }
                ImGui::EndTable();
            }
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit],[inspect],[ui],[run_model]"`
Expected: all pass. (`IM_ASSERT` throws, so an unbalanced `TreeNode`/`TreePop` or `BeginDisabled`/`EndDisabled` fails the `[ui]` tests.)

Then: `ctest --preset tests`
Expected: all pass.

- [ ] **Step 11: Commit**

```bash
git add src/app/model/SettingsEdit.h src/app/model/SettingsEdit.cpp src/app/model/Inspect.h src/app/model/Inspect.cpp src/app/ui/Windows.h src/app/ui/SettingsWindow.cpp src/app/ui/TurnLogWindow.cpp src/app/ui/MemoryWindow.cpp tests/app/SettingsEditTests.cpp tests/app/InspectTests.cpp tests/app/UiTests.cpp
git commit -m "Show the narration review and scene retrieval in the app" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
### Task 10: Documentation and milestone check

The architecture spec and README describe the review, the renarrate, the compact transcript and scene retrieval. `CLAUDE.md` does not change. A last full run under AddressSanitizer closes the milestone.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `README.md`

**Interfaces:**
- Consumes: every name above.
- Produces: documentation only.

- [ ] **Step 1: Update the architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md` (read it first; each old text below is unique):

Replace

```markdown
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window.
- Every command ends with exactly one closing event. `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep and close nothing.
```

with

```markdown
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `NarrationRestarted`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window. `NarrationRestarted` carries the review's problems and says that the running turn's narration starts over: the `TokenStreamed` events after it are the new narration.
- Every command ends with exactly one closing event. `TurnResolved`, `TokenStreamed` and `NarrationRestarted` report a running turn's progress; `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep. None of them close a command.
```

Replace

```markdown
Embed the player input. Rank lore entries, folded past turns and (in `assisted` mode) notes by cosine similarity, boosting lore subjects named in the input.
```

with

```markdown
Embed the query: the player input, the current scene's values joined by `; ` (see "World data"), then the previous turn's digest, one per line and cut to 4000 bytes with the input first. Rank lore entries, folded past turns and (in `assisted` mode) notes by cosine similarity, adding 0.15 once for a lore subject named in the input or in a scene value (the hit's reasons: `name`, `scene`); the digest only enters the embedding. Assisted-mode lookups search with their own query only.
```

Replace

```markdown
5. **Commit (engine).** Write the turn row, the state snapshot and any `assisted`-mode notes in one transaction.
```

with

```markdown
5. **Review (LLM, JSON grammar), when `narration_check` or `compact_transcript` is on.** A standalone call, not an extension of the narrate prompt: the review rules as the system message, then the player input, the outcome rendered as the narrate message renders it, and the narration. It is about 300 to 600 tokens whatever the run's length, so it needs no reservation in the context fit. Its grammar (`turn::BuildReviewGrammar`) allows `consistent` and up to 3 `problems` of up to 200 characters (the check) and a `digest` of up to 300 characters (the compact transcript). Output that cannot be read (cut off at `review_max_tokens`) is a model mistake: the turn goes on as if the review had found nothing. On Qwen3-4B-Instruct the review prompt replaces the narrate prompt in the backend's cache, so the next resolve call evaluates its whole prompt; Qwen3.5 and later reuse no prefix anyway.
6. **Renarrate (LLM, streamed prose), at most once, with the check on.** When the first review says the narration contradicts the outcome (a failed check narrated as a success or the reverse, an applied change left out or reversed, a rejected change narrated as if it happened), the worker sends `NarrationRestarted` and the narration is written again from the same narrate prompt, whose narrate message gains the review's problems before the narrate instruction. It stays within `narrate_allowance`, the problem lines shed last. The second narration is reviewed, and it and that review's digest are committed whatever the verdict: there are no loops.
7. **Commit (engine).** Write the turn row (with the last review's digest), the state snapshot and any `assisted`-mode notes in one transaction.
```

Replace

```markdown
- Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number).
```

with

```markdown
- Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number). `SamplingSeed` also takes the call's `core::CallKind`: `Lookup`, `Resolve`, `Narrate`, `Summarize`, `Review` and `Renarrate`, the last two appended so that existing seeds keep their values. A turn's second review seeds with its attempt plus `REVIEW_RETRY_OFFSET` (2^23), so it never meets a regenerated turn's first review.
```

Replace

```markdown
**Cancel and failure:** a cancelled or failed turn commits no turn.
```

with

```markdown
**Cancel and failure:** a cancelled or failed turn commits no turn, a cancel during the review or the renarrate included.
```

Replace

```markdown
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14/16 failure; player.location = "deck"]`). Full resolve JSON stays in the database.
```

with

```markdown
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14/16 failure; player.location = "deck"]`). Full resolve JSON stays in the database.
- **Digests.** With `compact_transcript` on, only the newest `prose_turns` entries show their narration; an older entry shows its turn's digest (the review's one or two sentences), or its narration when it has none (played with `compact_transcript` off, or its review failed). Each turn, the entry that crosses out of the newest `prose_turns` changes form, so a prefix-reusing model re-evaluates the transcript from there (about `prose_turns + 1` entries); the models without prefix reuse gain from the shorter transcript on every call. Fold calls always see the full narration, and the Play window shows it.
```

Replace

```markdown
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type, the token budget (including `retrieval_cap` and the per-call thinking budgets) and an optional `memory_mode`.
```

with

```markdown
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type, the token budget (including `retrieval_cap`, the per-call thinking budgets and the review's `review_max_tokens` and `review_thinking_tokens`), `narration_check`, `compact_transcript`, `prose_turns` (at least 1) and an optional `memory_mode`.
```

Replace

```markdown
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode; unknown keys are errors
```

with

```markdown
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode, optional scene;
                      unknown keys are errors
```

Replace

```markdown
| `turns` | n, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, timings |
```

with

```markdown
| `turns` | n, attempt, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, digest, timings |
```

After the table row `| \`embeddings\` | kind, ref id, embedding model, content hash of the embedded text, vector |` and the blank line that follows it, insert:

```markdown
The schema version is 2 (the `digest` column; it also covers `attempt`). A run written with another version is refused, without migration.

**Scene (optional).** `"scene": {"paths": ["player.location", "crawler.location"], "present_at": "player.location"}` names the string fields (`entity.field`, of entities in the initial state) that say where the player is; `present_at` is one of them. Each turn, `turn::SceneOf` lists the value at each path, then every other entity whose field of `present_at`'s name holds the same value (trimmed, case-insensitive): its id and its other string fields. Values are deduplicated case-insensitively. Retrieval searches with them and boosts the lore they name. Matching is by containment, like the name boost: "the old oasis" names the subject `oasis`, while "Iron Convoy depot" does not name `faction iron convoy`; such cases rely on the embedding.

```

Replace the three UI table rows

```markdown
| Turn log | Resolve JSON, rolls, applied and rejected changes, timings per turn |
| Memory | Summaries, notes, and last retrieval hits with scores |
| Settings | Models (picked from `models/`), context size, thinking budgets, sampling, `memory_mode` |
```

with

```markdown
| Turn log | Resolve JSON, rolls, applied and rejected changes, each review's verdict, problems and digest, the narration a renarrate replaced, timings per turn |
| Memory | Summaries, notes, last retrieval hits with scores and reasons, and the turn's scene values |
| Settings | Models (picked from `models/`), context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |
```

Replace

```markdown
Replay fixtures live in `tests/data/replays` and are recorded from real play with `morpheus_play --record`.
```

with

```markdown
Replay fixtures live in `tests/data/replays` and are recorded from real play with `morpheus_play --record`. A fixture records the run's `narration_check`, `compact_transcript` and `prose_turns` and its `review` replies; a renarrate's reply is among the `narrate` replies, in call order.
```

- [ ] **Step 2: Update the README**

In `README.md` (read it first):

Before the bullet that starts ``- `gpu_layers` is how many of the chat model's layers``, insert:

```markdown
- `narration_check` (true or false): after each narration a short review call checks it against the engine's outcome. A narration that contradicts it (a failed check told as a success, a change left out or reversed, a rejected change told as if it happened) is written once more and replaces the first.
- `compact_transcript` (true or false) and `prose_turns` (at least 1): the review also writes a one or two sentence digest of each turn, and in the prompt only the newest `prose_turns` turns keep their full narration; older ones show the digest. The model then copies its own prose less, and the transcript grows more slowly. The Play window always shows the full story.
- `budget.review_max_tokens` and `budget.review_thinking_tokens` are the review call's answer and thinking budgets (0, or at least 64, like the others). The review prompt is short and standalone, so they are not reserved in the turn prompt's context.
```

Replace

```markdown
Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn.
```

with

```markdown
Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn. When the review finds that a narration contradicts the outcome, the text clears and the rewritten narration streams in its place.
```

Replace

```markdown
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups and per-call token counts and timings, and the raw resolve output.
```

with

```markdown
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups, the narration review (each review's verdict, problems and digest, and the narration a rewrite replaced), per-call token counts and timings, and the raw resolve output.
```

Replace

```markdown
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores.
```

with

```markdown
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores and why each hit matched (`name`: named in your input; `scene`: named by the scene), and the scene values it searched with.
```

Replace

```markdown
context size, GPU layers, the resolve and narrate thinking budgets (0 for a model without a thinking mode), sampling and memory mode.
```

with

```markdown
context size, GPU layers, the resolve, narrate and review thinking budgets (0 for a model without a thinking mode), sampling, memory mode, the narration check, the compact transcript and its prose turns.
```

After the line ``- `/quit`: leave``, insert a blank line and:

```markdown
When the review finds that a narration contradicts the outcome, the terminal prints `[narration rewritten: <problems>]` and streams the new narration below it.
```

Before the line `## Layout`, insert:

````markdown
## Worlds

A world is a folder `game_projects/<id>/`: `world.json` (id, title, roll expression, check entity, stats, difficulties, default memory mode), `rules.md`, `lore.csv`, `state_schema.json` and `initial_state.json`. `world.json` may also declare a scene, the state values that say where the player is:

```json
"scene": {
  "paths": ["player.location", "crawler.location"],
  "present_at": "player.location"
}
```

Each path is `entity.field`, a string field of an entity in `initial_state.json`, and `present_at` is one of the paths. Every turn, retrieval also searches with those values and with the entities at `present_at`'s value (an NPC standing where the player stands, with its other string fields, such as its role), so lore about the place reaches the prompt even when the input names nothing. A world without a scene searches with the input and the previous turn's digest.

````

- [ ] **Step 3: Check the whole milestone**

Run: `ctest --preset tests`
Expected: all pass.

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: all pass, with no AddressSanitizer report.

Run: `git status --short`
Expected: only the two documentation files are modified; `settings.json` shows nowhere (it is gitignored), and nothing under `models/`, `dotnet/`, `.superpowers/` or `docs/references/` is staged.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md
git commit -m "Document the narration review, compact transcript and scene retrieval" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| Review call after narrate when `narration_check` or `compact_transcript` is on; none when both are off | 4 |
| Standalone review prompt: `REVIEW_RULES`; input, outcome rendered like the narrate message, narration, `REVIEW_INSTRUCTION` | 3 (`ReviewPrompt`, `RenderOutcome`), 4 |
| `BuildReviewGrammar(bool check, bool digest)`; `consistent`, `problems` (<= 3 x 200 chars), `digest` (<= 300 chars) | 3 |
| `ParseReviewOutput` -> `Review { consistent, problems, digest, error }`; unparseable = model mistake | 3, 4 |
| Renarrate once: `onNarrationRestarted` -> `NarrationRestarted`; Play window clears; `morpheus_play` line | 4, 5 |
| Renarrate prompt: correction paragraph before `NARRATE_INSTRUCTION`, within `narrate_allowance`; seed `CallKind::Renarrate`; `narrate_thinking_tokens`; streams | 3, 4 |
| Second review seeded `attempt + REVIEW_RETRY_OFFSET`; `PlayTurn` requires `attempt < REVIEW_RETRY_OFFSET` | 4 |
| Commit the second narration and digest whatever the verdict; no loops | 4 |
| `CallKind::Review` / `Renarrate` appended after `Summarize`; seed tests | 1 |
| Cancel during review or renarrate commits nothing | 4 |
| Settings `narration_check`, `budget.review_max_tokens`, `budget.review_thinking_tokens` (0 or >= `MIN_THINKING_TOKENS`) required | 1 |
| `settings.example.json`, this machine's `settings.json`, `SettingsEdit`, Settings window (check, review thinking, read-only `review_max_tokens`) | 1, 9 |
| Telemetry `review` (one or two entries, with `consistent`, `problems`, `digest`, `error`), `renarrate` with `replaced` | 4 |
| Turn log shows verdicts, problems, digests and the replaced narration | 9 |
| `TurnRecord::digest`, `digest TEXT NOT NULL`, `SCHEMA_VERSION` 2, version-1 refusal naming file and both versions | 2 |
| `RenderTranscript(turns, proseTurns)`: newest prose, older digests, empty digest falls back to narration | 6 |
| Session passes `proseTurns` (compact on) or `INT_MAX` (off); folding always `INT_MAX` | 6 |
| `ENGINE_RULES` sentence about older turns | 6 |
| Settings `compact_transcript`, `prose_turns` >= 1; Settings window edits both | 1, 9 |
| Prefix stability, PromptBuilder token drop, folding sees full narration | 6 |
| world.json `scene` block, `SceneDecl`, loader `DataError`s naming `world.json`; sandcrawler declares it, mini does not | 7 |
| `SceneOf(world, state)`: path values, present entities with id and other string fields, trim/lower-case match, dedup | 8 |
| Query: input, scene joined by `"; "`, previous digest, newline-joined, cut to `EMBED_TEXT_CHARS`; lookups unchanged | 8 |
| `Retrieve` takes the parts; `NAME_BOOST` once for input or scene; digest no boost; `reasons`; ranking and trims unchanged | 8 |
| Telemetry `retrieval[].reasons`, top-level `scene`; Memory window shows reasons and scene | 8, 9 |
| `CallKindOf` recognises the review grammar; fixtures gain `review` replies by hand and record the review settings | 5 |
| Headless UI: Turn log with a renarrated turn, Memory window with scene reasons | 9 |
| Architecture spec and README updates; CLAUDE.md unchanged | 10 |
