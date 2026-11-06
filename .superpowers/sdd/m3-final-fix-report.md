# M3 final review: fix report

Status: DONE_WITH_CONCERNS (concerns at the end; none blocks closing M3).

Commits (branch `cpp`, on top of 09dcebf):
- `1972d5b` Bound fold and condense calls by the context, with the engine summary as a fallback (I1)
- `0069465` Choose the content marker per call instead of reserving U+001E (I2)
- `e29509f` Close the M3 review's minor findings and ledger fix rows (Minor 1, 2, 3, 9 and every ledger "Fix" row)

## I1. Fold and condense calls must fit the context

What changed:
- `src/turn/Folding.{h,cpp}`: four new public functions, each with two callers:
  - `SummarizePrompt`, used by `SummarizeTurns` and by `Session::FoldOldest` to measure;
  - `SummarizeFallback`, used by `SummarizeTurns` for an empty reply and by `FoldOldest` when no turn fits;
  - `CondensePrompt` and `CondenseFallback`, used the same way by `CondenseSummaries` and `Session::Condense`.
  The fallback texts are unchanged.
- `Session::FoldOldest` measures `CountPromptTokens(SummarizePrompt(system, oldest k)) + maxTokens <= ContextSize()`.
  - It first tries the oldest half. If that does not fit, a binary search finds the longest prefix that fits.
  - If not even one turn fits, it writes that turn's summary with `SummarizeFallback` and makes no model call.
  - Every call folds at least one turn.
- `Session::Condense` measures `CondensePrompt(system)` the same way. When it does not fit, it uses `CondenseFallback` without a model call.
- `tests/fakes/ScriptedBackend.h`: `Complete` checks `maxTokens > 0`, then throws `InferenceError` (the same message as `LlamaCppBackend`) when the prompt plus `maxTokens` exceeds `ContextSize()`. The check runs before the call is recorded.
  - It exposed no existing test: all 191 previous `tests` cases still passed with it. The only failures it produced were in the new tests below.
- The spec's maintenance section describes the fit rule and the fallback. Session.h comments too.

New tests (`tests/turn/SessionTests.cpp`, `[session]`):
- "A run loaded with a much smaller context folds what fits and stays playable": 12 turns at context 16384, then `Load` at 3000. The transcript is about 6200 tokens, twice the context. Two sections: the next turn first, or `Maintain` first. Both then play on and check `Fits()`.
- "A turn too long to fold with a model call gets the engine's summary": a turn of about 3000 tokens, then `Load` at 3000. The summary is the engine text `Turn 1: I walk. [player.location = "lighthouse"]`.
- "A summary too big to condense with a model call gets the engine's condensed summary": two 6000-byte summaries written into the run file, then `Load` at 3000. No condense call is made, and the result covers turns 1-2.
- "Undoing below a condensed summary puts back a long transcript that still plays": summary_cap 16 condenses into one summary covering turn 1 through K (K >= 11). Undoing below K puts K-1 turns back. Turn, Maintain and turn all succeed.
- `tests/turn/FoldingTests.cpp` "The fold and condense prompts and fallbacks are the ones the folding calls use": the prompts match what the calls send, and the fallbacks match the empty-reply output.

RED (`morpheus_tests.exe "[session]"`, with the new tests and the enforcing ScriptedBackend, before the Session change):
```
SessionTests.cpp(596): FAILED:  prompt (3560 tokens) plus max_tokens (128) exceeds the context size (3000)
SessionTests.cpp(658): FAILED:  prompt (3650 tokens) plus max_tokens (4) exceeds the context size (3000)
SessionTests.cpp(556): FAILED:  prompt (3650 tokens) plus max_tokens (128) exceeds the context size (3000)
SessionTests.cpp(562): FAILED:  prompt (3650 tokens) plus max_tokens (128) exceeds the context size (3000)
SessionTests.cpp(567): FAILED:  (summaries.size() >= 2: 0 >= 2)
SessionTests.cpp(626): FAILED:  prompt (3552 tokens) plus max_tokens (256) exceeds the context size (3000)
test cases:  25 |  21 passed | 4 failed
```
The FoldingTests case was written together with the new Folding functions, so its RED was compile-only.

GREEN: `morpheus_tests.exe "[session],[folding]"`: All tests passed (365 assertions in 33 test cases). `ctest --preset tests`: 196/196.

Design note: the fold check uses the backend's exact `CountPromptTokens` on the real fold prompt, the same figure `Complete` checks. A binary search was chosen over a linear scan to keep token counts at O(log n) when a long transcript is restored.

## I2. No reserved content character

What changed (`src/llama/LlamaCppBackend.{h,cpp}`):
- `TokenizePrompt` picks a marker per call. It is the first of `"\x1E"`, `"\x1E1\x1E"`, `"\x1E2\x1E"`, ... that no message content contains.
  - The search ends: each occurrence of a candidate starts at its own 0x1E byte, so the contents can block only finitely many candidates.
  - The split now advances by `marker.size()`.
  - The "reserved character U+001E" throw is gone.
- A marker found in the template's own text would still be caught by the existing "repeats" and "drops" checks.
- Comments updated: the constant's comment, the TokenizePrompt header (it now documents the drop and repeat errors) and `Session::RunTurn`'s input stripping (kept, now described as hygiene).

Test (`tests/llama/LlamaHardeningTests.cpp`):
- The old `CHECK_THROWS_AS` on `"a\x1Eb"` was removed.
- New case: "Message contents holding U+001E or later marker candidates tokenize as plain text". Two messages: `"a\x1Eb"` and `"c\x1E1\x1Ed\x1E2\x1Ee"`, so the backend must skip to the third candidate. Their prompt-token delta equals the sum of their plain-text `CountTokens`.

RED (dev Debug, models exported, before the backend change):
```
LlamaHardeningTests.cpp(46): FAILED:
  CHECK( backend.CountPromptTokens(marked) - backend.CountPromptTokens(empty) == backend.CountTokens(first) + backend.CountTokens(second) )
due to unexpected exception with message:
  a message contains the reserved character U+001E
test cases: 1 | 1 failed
```
GREEN: `morpheus_tests.exe "[llama]"`: All tests passed (86 assertions in 21 test cases).

Observation: contents are replaced by the marker before rendering, so content bytes never reach the rendered string. The old throw was stricter than the splitting needed. The marker is still chosen absent from every content, as the brief asks, which also covers a template that would copy part of a content.

## Minor findings fixed

**Minor 1: unreadable-output reason in the record.**
- When `proposal.error` is set, `PlayTurn` writes it to `telemetry["resolve"]["error"]`.
- Test "The resolve telemetry records why the output could not be read" covers three cases: cut off, wrong shape (valid JSON, so `resolve` holds no "unparsed"), and readable output (no error key).
- RED: both error sections failed (`!true`).

**Minor 2: assisted lookups and retrieval_cap.** The first option was taken: lookups get only what is left of the cap.
- `RetrievalHit` has a new `tokens` field: the line cost `Retrieve` charged against the cap.
- `PlayTurn` starts `capLeft` at `retrieval_cap` minus the automatic hits, and subtracts each hit a lookup adds.
- At commit, each lookup request gets `"kept"`: the added refs that survived the fit loops and reached the resolve prompt.
- README and the spec's step 1 now say this.
- Tests:
  - new "Assisted-mode lookups share retrieval_cap with the automatic hits" (cap 15; the storm hit costs 8; keeper's 12 no longer fits);
  - the existing assisted test also checks `kept`.
- RED: `record->timings.at("retrieval").size() == 1` failed as `2 == 1`, and the `kept` lookup threw "key 'kept' not found".

**Minor 3: "nothing is committed" wording.** Reworded to: no turn is committed; memory upkeep finished before the stop is kept. This covers:
- Session.h (`RunTurn`);
- a new "Cancel and failure" paragraph in the spec;
- the Replay.h `RollBack` rationale, which also takes ledger row 14.

The repo has no "Cancel discards the turn" text. That phrase is only in `.superpowers/sdd/global-constraints.md`, which I did not edit.

**Minor 9: replay coverage.**
- `TurnResultJson` now includes `attempt` and `notes`.
- Both existing fixtures gained `attempt` and `notes` in their expected turns:
  - `sandcrawler-playtest.json` was rewritten with Python `json.dumps(indent=2, sort_keys=True)`. A round trip of the original file was byte-identical, so the diff is only the new keys.
  - `mini-undo-regenerate.json` was edited by hand.
- New hand fixture `tests/data/replays/mini-check-regenerate-note.json` (assisted mode, seed 7):
  - a `CLIMB`-style check, then a regenerate with a note: the replay pins rolls [15], total 18 vs 16, success branch applied, and attempt 1;
  - an assisted note from a lore lookup;
  - a notes lookup;
  - a rejected change ("no item 'oil' to decrease").
- The MakeReplayFixture test also checks the new keys.
- RED: all three fixtures failed their turn comparisons (missing keys).
- GREEN: `[replay]` all passed (488 assertions in 7 test cases on dev Debug).
- The new fixture's expected values were taken from the engine's first run and checked by hand: roll 15 + agility 3 = 18 >= 16, so success.

## Ledger "Fix" rows

| Row | Change |
|---|---|
| 6 perf: legacy-narrating comment | LlamaCppBackend.cpp sampler comment now says what it does ("so its samplers' state follows the output"). |
| 8: every Embed InferenceError became DataError | `MemoryIndex::Vectors` wraps only Lore (message "lore.csv entry '...'"). Turn and note failures stay `InferenceError`. Retrieval.h documents it. New test "A history text that cannot be embedded is a runtime failure, not a world error". RED: `memory '1' cannot be embedded: ...` (DataError). GREEN after the change. |
| 10: dev not rebuilt | `dev` Debug and RelWithDebInfo were rebuilt after all changes: tests, probe and play binaries are dated 2026-10-28 12:32-12:34. `morpheus.exe` was reported up to date by MSBuild: it is built from `src/main.cpp` and links only llama.cpp, none of which changed. |
| 12: Worker comment on maintenanceDue | Worker.cpp now lists "when a Cancel stops it with no command queued". |
| 13: hand fixture lacks checks and rejections | See Minor 9. |
| 14: Replay.h RollBack comment | Reworded (see Minor 3). |
| 14: main.cpp header | `tools/play/main.cpp`: "rolled back out of the recording, except summary replies". |
| 14: README /quit wording | A summary call cancelled at /quit "is not recorded at all". The contradictory "cut short by /quit ... never breaks a replay" clause is gone. |
| 15: CLAUDE.md preset ordering | `dev` is introduced before `dev-asan`. `dev-asan` is described as `dev` with CPU llama.cpp, ASan, RelWithDebInfo, building the tests, probe and play but not the app. |
| 15: README Vulkan note | The prerequisites say `dev-asan` needs no Vulkan SDK. |
| 15: dev-asan builds probe and play, not the app | One clause each in the README table (row moved after `dev`) and CLAUDE.md. |

## Verification (final tree, e29509f)

- `ctest --preset tests`: 100% passed, 199/199.
- `ctest --preset asan`: 100% passed, 199/199.
- `cmake --build --preset dev` (Debug) and `cmake --build build/dev --config RelWithDebInfo`: both succeeded, with no warnings or errors under /W4 /WX.
- `ctest --preset dev` with both model variables exported in the same command: 100% passed, 220/220 (192.6 s).
- Probe (`morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."`):
  - 723-token prompt; run 1: 0 reused; run 2: 722 reused, 1 evaluated.
- `[replay]` (dev Debug): all passed, 3 fixtures including the new hand fixture.

## Deferred, with reasons

- **Minor 4 (SCHEMA_VERSION 1 to 2):** this is a data-format change. Bumping makes every run file created by M3 code (which already has the M3 schema) fail the version check. The implementer instructions say to stop on data-format changes, and the review lists it as deferrable. It needs an owner decision: bump with a migration, or accept losing M3 runs.
- **Minor 5 (SyncHistory at the start of PlayTurn):** a behavior change to retrieval freshness. The review marks it deferrable, and the worker ordering already covers the normal path.
- **Minor 6 (regenerate restore in one transaction or savepoint):** needs a RunStore transaction API. The review itself suggests M4.
- **Minor 7 (`request_stop()` outside `_mutex`):** no stop callbacks exist today. Deferrable per the review.
- **Minor 8 (trim and drop empty notes):** deferrable per the review. It would change committed records.
- **Minor 10 (KV-prefix stability test):** deferrable per the review. The probe and playtest show the reuse.
- All ledger rows marked "Defer" were left as they are.

## Concerns

- **Tool-call quoting.** When files were edited through Python heredocs in the Bash tool, `\\` arrived as `\`. This broke one test comment, which I caught and fixed. Later edits were done through script files or the Edit tool. I checked every committed diff for stray control bytes or broken lines.
- **Shrinking `context_size` can drop history detail.** The fold fallback makes no model call. Its summary is only the turns' inputs and mechanics lines, so narration detail is lost from L2. Folded turns are still embedded and retrievable. Nothing logs or flags that the fallback ran; the M4 inspector may want a telemetry flag.
- **The [llama] test checks counting only.** It verifies plain-text tokenization through `CountPromptTokens`. `Complete` uses the same `TokenizePrompt`, so it is covered by construction rather than by a generation test.
