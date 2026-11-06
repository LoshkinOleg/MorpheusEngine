# Milestone 5 global constraints and decisions

Verbatim from the plan docs/superpowers/plans/2026-11-06-narration-review-and-scene-retrieval.md (Global Constraints, then the choices the plan made where the spec is silent). Spec: docs/superpowers/specs/2026-11-06-narration-review-and-scene-retrieval-design.md.

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

## Decisions the plan made (binding)

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
