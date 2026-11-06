# Milestone 3 final review (ca554b4..09dcebf)

Reviewer scope: whole-branch view of the turn layer (`src/turn`), the carry-over hardening in core, store, inference, context and llama, `tools/play`, the replay fixtures, the `dev-asan` preset, and the docs (spec, README, CLAUDE.md). Per-task reviews and the ledger's test results were taken as given. No suites were re-run and no code was changed.

## Strengths

- **The fit loop matches the plan.** `Session::AssembleFitting` follows the plan's order exactly: shed hits while the tail is over, condense, fold, last-resort shed, then `TurnError`. Its termination argument is written down and enforced: `CondensedOverCap` stops a second condense. The tests cover each branch with tight-context fakes, including a runaway-condense guard.
- **Regenerate and undo stay consistent.**
  - Truncation and the index resync happen together.
  - A cancelled or failed attempt puts the previous turn back with its exact summaries.
  - The index is resynced before the attempt retrieves, so the replaced attempt is never retrieved by its replacement.
  - Each of these has a test.
- **The KV-cache design holds up end to end.**
  - Narrate only appends to the resolve prompt (tested).
  - Fold and condense prompts start with the resolve prompt's system message.
  - Maintenance only counts tokens and embeds on the separate embedding context, so it never disturbs the chat cache between turns.
  - The playtest's 1886/1887 reuse confirms it.
- **Generated GBNF is a good call.**
  - It is unit-tested without a model and has a structural checker.
  - It is compiled and exercised by a `[llama]` test.
  - It bounds every list and string, which in turn bounds the narrate message.
  - Held counter items are matched by name, so world-authored item names work.
- **Content-safe tokenization.** Contents are tokenized without special-token parsing, so player text such as `<|im_end|>` cannot forge a turn boundary. The `[llama]` test proves this at the token level.
- **The Worker contract is precise and correct.**
  - Every command gets exactly one closing event.
  - Cancel is sequence-numbered, so a cancel that races a Post is handled.
  - Maintenance can be preempted, except before retrieving commands.
  - `_thread` is declared last, so it is joined first, and the worker thread closes the run.
  - A `stop_callback` cancels the running job on shutdown.
  - No data race or lifetime problem found.
- **RecordingBackend and `morpheus_play`:**
  - The mutex covers only the recording, never the inner call.
  - The mark is taken before the Post.
  - Summaries are deliberately kept through a rollback.
  - The fixture is refused after a maintenance failure.
  - The replay test fails on unused non-summary replies.
- **Error classes are mostly consistent** across modules: `ContractError` for engine bugs (the worker prefixes "engine bug:"), `DataError` for settings and world limits, `TurnError` for player-facing refusals, and model mistakes as values (unreadable output, rejected checks, lookup errors in telemetry).
- **Tests are strong overall.** The Worker tests use generous waits and have no timing-dependent assertions apart from the 500 ms shutdown bound, which has a 2 s margin. Dice outputs are pinned by golden values (`DiceTests` "Dice outputs are pinned").

## Critical

None.

## Important

### I1. Fold and condense calls never check their own prompt size, so a long transcript can make every turn fail

**Where:**
- `src/turn/Session.cpp:447-465` (`FoldOldest`) and `:467-479` (`Condense`)
- `src/turn/Folding.cpp:15-47` (`SummarizeTurns`) and `:49-77` (`CondenseSummaries`)
- `src/llama/LlamaCppBackend.cpp:219-222`: the backend throws `InferenceError` when prompt + `maxTokens` > `n_ctx`
- `tests/fakes/ScriptedBackend.h:76-108`

**Problem.** `FoldOldest` always folds the oldest half of the transcript (at least one turn) in a single call. Its prompt is the system message, then those turns, then the instruction. Nothing checks that this prompt plus `summary_cap / 4` fits the context.

The fit loop only measures the resolve prompt. That prompt is allowed to be far over budget, since being over budget is just a flag. The transcript can grow well past the context in two ways:
- **Shrinking `context_size` between sessions.** `settings.json` is per machine and runs are portable. Example: a run played at 32k context, with a transcript of about 25k tokens, is loaded with 8-12k. Folding half of it is then about 12k tokens plus the system message.
- **Undoing past a condensed summary.** `TruncateAfter` deletes every summary that ends after the target turn. Once all summaries are condensed into one summary covering turns 1..K, undoing to below K puts every turn from 1 to K-1 back into the transcript.

**Consequence.** `LlamaCppBackend::Complete` throws `InferenceError` for the fold call. That happens:
- in every turn's fit loop, which emits `Error`;
- in every maintenance pass, which emits `MaintenanceFailed`.

The run cannot be played until `context_size` is raised again. This also breaks the error policy, which says a turn that cannot run is a `TurnError` and a settings problem is a `DataError`. The same gap exists for `Condense` when L2 is huge (for example, after `summary_cap` was lowered), but that is less likely.

**Why the tests miss it.** `ScriptedBackend::Complete` does not enforce the backend's contract (prompt + `maxTokens` <= `ContextSize()`). No fake-backed test can see an oversize memory call: "Folding keeps every prompt within the context" only checks `LastAssembly().Fits()`.

**Fix:**
- In `FoldOldest`, fold the longest prefix of the oldest half whose prompt fits. Measure it with `CountPromptTokens(system + turns + instruction) + spec.maxTokens <= ContextSize()`.
  - It always folds at least one turn.
  - If one turn alone does not fit, throw `TurnError` or `DataError`; do not let the call reach the backend.
  - The loop then simply folds more often.
- Give `Condense` the same check, as a `DataError` naming `summary_cap`.
- Make `ScriptedBackend::Complete` throw `InferenceError` exactly as `LlamaCppBackend` does.
- Add tests:
  - build a long history in a large context, then `Load` it with a small context, and check that the next turn and `Maintain` succeed;
  - condense, undo below the condensed summary, then play a turn.

### I2. U+001E is a reserved character, but only player input is cleaned

**Where:**
- `src/llama/LlamaCppBackend.cpp:30` and `:521-560` (`TokenizePrompt`)
- `src/turn/Session.cpp:109-118` (input stripping)
- `src/turn/ResolveGrammar.cpp:16`: the `char` rule allows `"\\u" [0-9a-fA-F]{4}`

**Problem.** Every prompt is tokenized by replacing each message content with `'\x1E'`. Any content that already contains that byte makes the backend throw `InferenceError`. `Session::RunTurn` strips it from player input, and that is the only guard. Text that reaches prompts without passing through that guard:
- **Narration.** It is sampled without a grammar and stored verbatim, and it re-enters every later prompt through L3 and history retrieval.
- **Grammar-constrained JSON strings.** The `char` rule excludes raw control bytes but accepts the escape `\u001e`. After parsing, the byte can end up in:
  - `intent` and `stakes` (the narrate message);
  - string field values (the state, and therefore T1 of every later prompt);
  - notes (retrieved in later turns).
- **World files.** `rules.md`, lore and initial-state strings are not checked for the byte. The failure surfaces as an `InferenceError` at the first prompt, not as a `DataError` naming the file at load.

**Consequence.**
- Once such text is committed, every later prompt that contains it throws `InferenceError`: each turn, the fold that would remove it from L3, and maintenance. The run is stuck until the player undoes that turn.
- This contradicts the error policy: model mistakes are values, and bad world data is a `DataError` naming the file.
- The likelihood is very low with Qwen3 at these sampling settings. The failure, however, is permanent, and the fix is local.

**Fix (in one place).** Stop reserving a character.
- In `TokenizePrompt`, choose a per-call sentinel that no content contains. For example, start with `"\x1E"` and append a counter or random bytes until no message contains it. Split on that sentinel.
- Drop the "reserved character" throw.
- Keep the input stripping as hygiene, and update its comment, which says U+001E is the backend's marker.
- Keep the `[llama]` test and change its expectation: a content containing `\x1E` tokenizes as plain text.

## Minor

1. **The unreadable-output reason is not stored with the turn** (`src/turn/Session.cpp:331-338`, `:365`).
   - `ParseResolveOutput`'s `error` only reaches the narrate prompt.
   - For JSON of the wrong shape, `record.resolve` holds normal-looking JSON with nothing applied, and nothing in the record says why. The M4 inspector reads the record.
   - The lookup path already stores `timings.lookup.error`. Mirror it: `telemetry["resolve"]["error"] = proposal.error` when it is not empty.
2. **In assisted mode, retrieved memory can exceed `retrieval_cap`** (`src/turn/Session.cpp:290-305`; `README.md:40`; spec turn-flow step 1).
   - Each lookup's `Retrieve` gets the full `retrievalCap`, so T2 can reach about 4x the cap. Only `tail_allowance` limits it.
   - The README says `retrieval_cap` "caps the retrieved memory ... placed in the prompt".
   - The lookup telemetry's `added` also lists hits that the resolve fit loop may later shed.
   - Fix one of two ways: pass lookups the cap that is left after the automatic hits, or document that each lookup has its own cap. Either way, record which lookup hits survived.
3. **"Nothing is committed" is not quite true** (`src/turn/Session.h:70-74`; spec "Cancel discards the turn"; `src/turn/Replay.h:60-65`).
   - A cancelled or failed turn keeps the summaries, and the history embeddings, that its fit loop folded or condensed before the stop.
   - This is harmless, because they only cover committed turns. But several comments and the RollBack rationale state the opposite.
   - Reword them to: no turn is committed; memory upkeep done before the stop is kept.
4. **`SCHEMA_VERSION` is still 1 after schema changes** (`src/store/RunStore.h:42`).
   - M3 added `turns.attempt` and the `note` embedding kind.
   - A pre-M3 run file passes the version check, then fails with a raw SQLite "no such column" `StoreError` instead of the clear version message. Bump it to 2.
5. **Retrieval freshness depends on the worker's scheduling** (`src/turn/Session.cpp:265`; `src/turn/Worker.cpp:157-167`).
   - `PlayTurn` never indexes the previous turn's notes itself. When maintenance fails (`MaintenanceFailed`) or a Cancel with nothing queued drops it, the next turn retrieves without those notes. The same applies to `Session` used without a worker.
   - Calling `_index.SyncHistory(_store, FoldedThrough(), *_pBackend)` at the start of `PlayTurn` makes the guarantee a property of `Session`. It is hash-checked, so it is cheap when nothing changed. The worker's ordering then becomes an optimization.
6. **Regenerate deletes the turn before replaying it** (`src/turn/Session.cpp:142-177`).
   - If the process dies during the attempt (for example, a llama.cpp or Vulkan abort), the original turn is lost on disk.
   - The restore itself is three separate writes: `CommitTurn`, `ReplaceSummaries`, then each `AddSummary`.
   - Wrap the restore in one transaction. Consider a savepoint over the whole attempt in M4.
7. **`Worker::Post` and `Worker::Cancel` call `request_stop()` while holding `_mutex`** (`src/turn/Worker.cpp:26-43`).
   - Stop callbacks registered on the job token run synchronously inside that lock. None exist today, because both backends poll the token.
   - A future backend that registers a `std::stop_callback` taking its own lock, or calling back into `Emit`, would deadlock.
   - Copy the `stop_source` under the lock and call `request_stop()` after releasing it.
8. **Empty notes are kept** (`src/turn/Session.cpp:342-344`). The grammar allows `""` (and whitespace-only notes), and `PlayTurn` commits them, which yields "Note from turn N: " entries. Trim notes and drop empty ones before committing.
9. **The replay fixtures pin less than they appear to** (`src/turn/Replay.cpp:143-159`; `tests/data/replays/*.json`).
   - Neither fixture has a check, assisted mode or a summary.
   - `TurnResultJson` leaves out `notes` and `attempt`.
   - So the end-to-end replay pins neither the dice wiring, nor notes, nor regenerate attempt numbers. The dice themselves are pinned by `DiceTests`.
   - Fix: add `notes` and `attempt` to `TurnResultJson`, and hand-write a mini fixture with a check (the `CLIMB` reply), a regenerate, and an assisted-mode note. This closes ledger item 13c.
10. **No test pins KV-cache prefix stability across turns or memory calls.**
    - A cheap ScriptedBackend test could assert that, without a fold:
      - turn N+1's resolve prompt starts with turn N's system message and transcript messages, unchanged;
      - a fold prompt's messages are a prefix of the preceding resolve prompt's messages.
    - Today only the manual playtest shows the reuse. A `[llama]` Session-level check, such as narrate `tokensReused` >= the resolve prompt's tokens, could follow in M4.

## Ledger Minor triage (Milestone 3)

| Task | Finding | Decision | Reason |
|---|---|---|---|
| 1 | Settings `is_regular_file` drops `error.message()` | Defer | Message wording only; access errors are still loud |
| 1 | `RunStore::Open` shadows `error` | Defer | Cosmetic; it builds under /W4 /WX |
| 1 | ShortJson only tested via StateSchema | Defer | Simple function; covered indirectly |
| 1 | CutUtf8 lacks 3/4-byte and maxBytes 0 tests; `Trim("")` untested | Defer | Implementation is correct by inspection; add tests when next touched |
| 1 | SamplingSeed test lacks the Lookup kind | Defer | Same code path as the other kinds |
| 1 | fs `error_code` paths untested | Defer | Hard to provoke portably; low value |
| 2 | Settings.h names the private SETTINGS_TEMPLATE_MARGIN | Defer | Doc nit |
| 2 | SettingsToJson test checks only 7 of about 20 fields | Defer | Meta is informational only |
| 2 | memory_mode "engine" / non-string and retrieval_cap 0 untested | Defer | Same validators as tested keys |
| 2 | Mode strings duplicated | Defer | Now in three places (Settings, World, Replay). Add a `MemoryModeName` when next touched |
| 2 | CurrentPathGuard dtor uses throwing `current_path` | Defer | Test helper |
| 2 | `absolute()` keeps ".." | Defer | Cosmetic in run meta |
| 3 | ReplaceSummaries test does not assert state after a rejected range | Defer | It is a REQUIRE before any write |
| 3 | Long SQL lines | Defer | Style |
| 3 | Note refs sort "2.10" before "2.9" | Defer | No consumer depends on the order |
| 3 | CAST orders numeric-prefixed lore subjects | Defer | Order is not semantic |
| 4 | Log sink exceptions are not contained | Defer to M4 | Must-not-throw is documented. Wrap it when the app installs the first real sink |
| 4 | Injection test RED was compile-only | Defer | Process note; the test is meaningful now |
| 5 | world_cap DataError text omits "game reference" | Defer | Wording |
| 5 | L1-order test REQUIREs only the reference | Defer | Order is also visible in other prompt tests |
| 5 | "Covers every change form" title overstates | Defer | Title only |
| 5 | Long transcriptBudget comment line | Defer | Style |
| 5 | Unused Errors.h include in WorldReference.cpp | Defer | Harmless |
| 5 | Empty difficulties would render "difficulty:." | Defer | Unreachable: world loading requires non-empty difficulties |
| 5 | Compile-RED skipped | Defer | Process note |
| 6 perf | Dead-end grammar throws llama's `runtime_error` | Defer | Generated grammars are structurally checked. Worker still reports it as Error; wrap in InferenceError when next touched |
| 6 perf | Legacy-narrating comment at LlamaCppBackend.cpp:340 | Fix | One line; CLAUDE.md forbids legacy narration |
| 6 perf | Seeded-determinism test weak | Defer | Replays do not depend on model determinism |
| 6 | No `[llama]` compile of a grammar with escaped held-item literals | Defer | Add a quoted or backslashed item to the `[llama]` grammar test when it is next run; world names like that are unlikely |
| 6 | GrammarProblem misses stray `]`/`}` and unused rules | Defer | Test helper; llama.cpp compile is the real check |
| 6 | No held name with backslash or control char tested | Defer | Same as above |
| 7 | INTENT_SHORT_CHARS counts bytes | Defer | Naming; behavior is UTF-8-safe |
| 7 | Redundant CountTokens when the intent is already short | Defer | Negligible |
| 7 | Multi-byte intent cut untested here | Defer | CutUtf8 is tested |
| 7 | Branch selection tested at one seed only | Defer | Seeds are pinned in DiceTests |
| 7 | Shed loop does O(n) tokenizations | Defer | n is at most 6 to 9 |
| 8 | Every InferenceError from Embed becomes DataError | Fix | History and note texts are engine-cut, so a failure there can only be a runtime fault, misreported as world or memory data. It can also mask the original error on Regenerate's restore path. Wrap Lore only |
| 8 | vectors-to-entries loop written 3x | Defer | Style |
| 8 | *_CHARS constants count bytes | Defer | Naming |
| 8 | SyncHistory/Retrieve lack body comments | Defer | Short and readable; add when next touched |
| 8 | Name boost is a substring match | Defer | Tune in play, with RETRIEVAL_MIN_SCORE |
| 8 | Tests miss min-score/sources filtering, short-circuits, repeated SyncHistory, continue-vs-break | Defer | Revisit with retrieval tuning (owner follow-up) |
| 8 | Embed `.at(0)` not REQUIRE'd | Defer | Backends return one vector per text; `.at` still throws |
| 8 | ScriptedBackend unsynchronized | Defer | Only read after the worker is idle (see 12) |
| 9 | stoppedAtLimit summaries stored without a signal | Defer | Quality only; surface in the M4 inspector |
| 9 | Fallback cut inside a multi-byte char untested at this level | Defer | CutUtf8 covers it |
| 9 | Fallback test uses RenderMechanicsLine, not a literal | Defer | Acceptable coupling |
| 10 | Restore SyncHistory can replace the original exception | Defer | The turn is restored first, so data is safe. The Task 8 fix removes the worst case (DataError masking InferenceError) |
| 10 | Load schema test calls LoadWorld inside CHECK_THROWS | Defer | Not vacuous today ("shore" is in the edited enum); hoist when next touched |
| 10 | summary_cap test does not check the message | Defer | Type is checked |
| 10 | summary_cap DataError does not name settings.json | Defer | Session does not know the path. Prefix with "settings budget." when next touched |
| 10 | Successful regenerate drops covering summaries | Defer | Rare (latest turn folded) and self-healing |
| 10 | dev not rebuilt after the fix round | Fix | `build/dev/bin/Debug` predates commits 254e0e9 and 9871bf3, and its morpheus.exe is from 2026-10-27. Run a clean `cmake --build --preset dev` before closing |
| 11 | Lookup test asserts only `added[0]` | Defer | Fake embedding makes the rest incidental |
| 11 | Untested: lookup dedupe, malformed lookup output, query cut, lookup-hit shedding | Defer | Low risk; malformed output is unit-tested in ParseLookupOutput |
| 11 | "Keeps no notes" test does not assert that notes are absent | Defer | Engine grammar has no notes; tighten by giving the reply a notes field |
| 11 | Lookup fit can shed automatic hits that resolve would fit | Defer | Assisted mode only; revisit with Minor 2 |
| 12 | Notes test reads Calls() while the worker may run maintenance | Defer | No fold, so no Complete call, so no race today |
| 12 | Run comment on when maintenanceDue clears is inaccurate | Fix | Worker.cpp:105 omits "Cancel with nothing queued". Threading comments must be exact |
| 12 | Untested: early-cancelled StartRun/Undo, pre-turn maintenance ignoring Post, Cancel dropping maintenance | Defer | The notes test covers the outcome. Add when M4 binds Cancel |
| 12 | Maintenance reruns once after Cancel during pre-turn maintenance | Defer | Harmless and bounded |
| 13 | Multi-key step dispatches the first match | Defer | Fixtures are generated; low risk |
| 13 | Summary-fallback WARN untested | Defer | Diagnostic only |
| 13 | Hand fixture has no checks or rejections | Fix | With Minor 9: one hand fixture with a check, regenerate and an assisted note makes replay pin dice and notes end to end |
| 13 | Recording round-trip untested | Defer | The sandcrawler fixture is a real round-trip |
| 14 | Replay.h RollBack comment overstates | Fix | It is self-contradictory ("never summaries" vs "rare"). Reword with Minor 3 |
| 14 | main.cpp header says "rolled back" without "except summaries" | Fix | One line |
| 14 | README "/quit cut short" wording | Fix | A summary cancelled at /quit is not recorded at all; say so |
| 14 | A stray summary could shift texts in a replay fold | Defer | Summaries never affect checked results |
| 14 | Recording failure paths untested against real models | Defer | Manual tool |
| 15 | CLAUDE.md preset ordering and wording around dev-asan | Fix | Doc pass: introduce dev before dev-asan |
| 15 | README Vulkan note | Fix | Doc pass: dev-asan needs no Vulkan SDK |
| 15 | dev-asan builds probe and play, not the app (undocumented) | Fix | Doc pass: one clause in README and CLAUDE.md |

The "Fix" rows are, apart from Task 8's narrowing and the 13c fixture, one-line comment or doc changes plus a rebuild. One commit can take them, together with Minor 3.

## Verdict

**With fixes.**

Close M3 after:
- I1: bound memory calls by the context, and make ScriptedBackend enforce the context like the real backend;
- I2: stop reserving U+001E in tokenization;
- the triage rows marked Fix.

Minor 1-10 can be fixed before closing or carried into M4. Minor 1 and 2 matter most to the M4 inspector, and Minor 9 to replay coverage.
