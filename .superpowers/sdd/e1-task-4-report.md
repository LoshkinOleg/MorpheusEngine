# E1 Task 4 report: the speaker's memories in the tail, the speaker's L3, and the speaker's fold

Status: DONE

Commit: b697d45 "Hosted prompts: the speaker's memories in the tail, the speaker's witnessed L3, and a fold during a turn is the speaker's memory"

## What was implemented

- `context::MemoryLine` and `context::RenderMemories(speakerName, memories)` ("# What <name> remembers", then "- <text>" or "- (told by <teller>) <text>"; "" for none).
- `HostedInputs` lost `summaries`, gained `speakerName` and `memories`. `AssembleHostedPrompt` no longer puts L2 in the system message (system = systemText + "\n\n" + worldText); the tail opens with the memories plus a blank line; `tokens.summary` counts the rendered memories; `LayOut(..., SummaryPlace::Tail, ...)`. Doc comments of `HostedInputs`, `AssembleHostedPrompt` and `RenderSummaries` (now owned-only) updated.
- `turn::HOSTED_SUMMARIZE_INSTRUCTION` and `turn::HOSTED_CONDENSE_INSTRUCTION` (verbatim, `{actor}`).
- `HostedSession`:
  - `Open` throws `core::DataError` ("settings: summary_cap N must be below tail_allowance M for a hosted run: ...") right after `HostedWorld world(data);`, before the file is touched.
  - `Entries(conversation, afterTurn, throughTurn, witnessId)`: range plus witness filter (speaker or listener); `previous` still follows every turn.
  - `Pass(conversationId, actorId, transcript, hits, inputs)`: memories from `ActorSummaries(actorId, false)`, teller names resolved via participants, then the observation's actors, then the id. `ActorsOf` guarded with `inputs.observation.is_object()`.
  - `Upkeep(conversationId, actorId, turn, foldsConversation)`: actor scope with filled hosted texts; storeFold uses `FoldConversation` or `RememberFold`; condense message = `RenderMemories(name, tellers by id) + "\n" + filled condense instruction`.
  - `RunTurn`: `speakerPass` builds L3 from `Entries(now, now.foldedThrough, LatestHostedTurn(), speaker)`, re-read each pass; `Upkeep(..., true)`.
  - `SyncHistory`: `Entries(conversation, 0, conversation.foldedThrough, "")`.
  - `Maintain`: folds as the last speaker with `Entries(now, now.foldedThrough, latest, lastSpeaker)` and `Upkeep(..., latest, true)`.

## Tests

- `tests/context/HostedPromptTests.cpp`: the brief's new test (helpers `Inputs()` / `RoomyBudget()`), plus a new test for the Tail branch of LayOut: memories over summary_cap make the raw tail exceed tail_allowance but `tailOverBudget` stays false (counted at the cap), `summaryOverBudget` is set, `transcriptBudget` equals the no-memories prompt's, and a player line over the allowance by itself still sets `tailOverBudget`. Existing tests moved from L2 in the system message to memories in the tail.
- `tests/hosted/HostedMemoryTests.cpp` (new): the brief's three tests.
- `tests/hosted/HostedTestSupport.h`: Responder recognises the hosted memory calls (brief's code), plus a helper `test::IsHostedFold(prompt)` (prefix of HOSTED_SUMMARIZE_INSTRUCTION before `{actor}`).
- `tests/hosted/HostedFoldingTests.cpp`: L2 expectations now look for "# What Hulda remembers" at the start of the tail; the Maintain fold and condense calls expect the filled hosted texts; the condense test's summary_cap went from 12 to 11 (one memory renders as 9 tokens, two as 12 with the new heading, so 12 no longer made two over the cap).
- `tests/hosted/HostedTurnTests.cpp`: one comment ("no summary yet" became "a hosted one has no L2").
- `tests/server/ApiTurnTests.cpp`, `tests/server/RunWorkerTests.cpp`: their fold detection compared with `turn::SUMMARIZE_INSTRUCTION`; now `test::IsHostedFold(prompt)`.

### TDD evidence

RED: `cmake --build --preset tests`
```
tests\context\HostedPromptTests.cpp(45,12): error C2039: 'speakerName': is not a member of 'morpheus::context::HostedInputs'
tests\context\HostedPromptTests.cpp(46,12): error C2039: 'memories': is not a member of 'morpheus::context::HostedInputs'
tests\context\HostedPromptTests.cpp(166,5): error C2039: 'RenderMemories': is not a member of 'morpheus::context'
...
```
After the implementation, first `[hosted]` run: the fold test failed (`foldedThrough >= 1` was `0 >= 1`) with 3000-byte lines; tuned to 5000 (see deviations).

GREEN (build with no warnings or errors):
```
[hosted]         All tests passed (928 assertions in 82 test cases)
[context]        All tests passed (236 assertions in 29 test cases)
[turn]           All tests passed (163 assertions in 16 test cases)
[replay]         All tests passed (507 assertions in 7 test cases)
[retrieval]      All tests passed (57 assertions in 11 test cases)
[hosted-server]  All tests passed (1071 assertions in 54 test cases)
ctest --preset tests: 100% tests passed out of 587 (4 [llama]/[server] tests skipped as usual)
```
No Session test, replay fixture or owned prompt was edited.

## Files changed

- src/context/PromptBuilder.h, src/context/PromptBuilder.cpp
- src/turn/EngineText.h
- src/turn/HostedSession.h, src/turn/HostedSession.cpp
- tests/context/HostedPromptTests.cpp
- tests/hosted/HostedMemoryTests.cpp (new), tests/hosted/HostedTestSupport.h, tests/hosted/HostedFoldingTests.cpp, tests/hosted/HostedTurnTests.cpp
- tests/server/ApiTurnTests.cpp, tests/server/RunWorkerTests.cpp

## Deviations from the brief

1. Fold test sizing: 3000-byte lines did not fold. With a 4096 context L3 gets about 2250 tokens, and two 750-token turns fit. Changed to 5000-byte lines (1250 tokens each) and noted the sizing in the test comment.
2. Upkeep: in the brief, `condenseMessage` captured `fill`, which captures the local `name` by reference. That reference would dangle once Upkeep returns, because the MemoryUpkeep (and its scope) outlives the call. The lambda now captures `name` and the pre-filled condense instruction by value.
3. `RunTurn` and `Maintain` check `_store.Conversation(...)` with MORPHEUS_REQUIRE before dereferencing, instead of the brief's unchecked `*_store.Conversation(...)`.
4. storeFold and storeCondensed take `const core::Summary&` and copy into a local, as the brief's note said.
5. Two server test files (outside the brief's `tests/context/` and `tests/hosted/` paths) were edited and committed. Their fold detection used the owned SUMMARIZE_INSTRUCTION, so `[hosted-server]` would have broken. Added the `test::IsHostedFold` helper for them and for the Responder.
6. Condense test summary_cap 12 -> 11 (see above).

## ModelSeat / Replay / CallKindOf

`turn::CallKindOf` (Replay.cpp, used by RecordingBackend for `morpheus_play --record`) and `app::SeatLabelOf` (ModelSeat.cpp, the app's model seat) recognise memory calls by the owned texts. Only the owned game uses them: no file in src/server or the hosted path uses CallKindOf, SeatLabelOf, ModelSeat or RecordingBackend, and the app does not open hosted runs. Left unchanged. Note: if a hosted fold ever went through SeatLabelOf, its Summarize branch would not apply. CallKindOf would return Narrate for a hosted fold, so SeatLabelOf would label it Narrate; it would not throw.

## D1 fixtures

conversation.json is spoken only by Hulda (Aela listens). radiant.json alternates Saadia and Hulda, both participants and listeners of each other's turns. So every speaker witnessed every earlier turn, and no `l3` expectation changed. HostedFixtureTests pass unedited.

## Byte checks

`LC_ALL=C grep -c $'[\x80-\xFF]'` and `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'` print 0 and 0 for all 12 changed files. Literal `\n` escapes checked with `grep -F` in PromptBuilder.cpp, HostedSession.cpp and HostedFoldingTests.cpp. One Python heredoc edit decoded `\\n` into real newlines in a test string; I caught it in the diff and fixed it with Edit before building.

## Concerns

- `Maintain` folds only the turns the last speaker witnessed, but `FoldConversation` moves the conversation's folded mark past any unwitnessed turns in between. Those turns become no one's memory until Task 5 replaces Maintain. This is as the brief specifies.
- The ASan suite was not run (the instructions ask only for `ctest --preset tests`).
- Nothing is left running (`tasklist | grep -i -E "morpheus|ctest"` is empty).

## Fix round (review of b697d45)

Commit: 1af9885 "Hosted memory fixes: the template's summary_cap below its tail_allowance, a test of the witnessed L3, {actor} filled once, tellers by name in the condense call"

### Changes

1. `settings.example.json`: `"summary_cap": 1024`, `"tail_allowance": 3072` (owner's choice). `settings.json` was not touched or staged. The test "settings.example.json loads with the current loader" now checks `summaryCap == 1024`, `tailAllowance == 3072` and `summaryCap < tailAllowance`. README's settings section quotes neither number, so it is unchanged.
2. New test "A speaker's L3 holds only the conversation's turns it witnessed":
   - Hulda and Aela play two turns, then Saadia joins.
   - Saadia's L3 (messages 1..stableCount) is empty and holds neither line.
   - Hulda's next L3 holds all six messages, Saadia's turn included.
3. `{actor}` fill scans forward (`at = filled.find(ACTOR_VARIABLE, at + name.size())`). New test "A hosted NPC's name that holds {actor} fills the hosted texts once": Hulda is named "{actor} the Bold", and her Maintain fold's instruction must equal the text with that name in each `{actor}` slot.
4. Minors:
   - Tellers by name in the condense message. A namespace helper `ActorName(participants, observed, id)` (participant, else observation actor, else id) now has two callers, Pass and Upkeep.
     - To resolve names as Pass does, `Upkeep` takes the observation: `Upkeep(conversationId, actorId, const nlohmann::json& observation, int turn, bool foldsConversation)`. This private signature differs from the brief's Produces. RunTurn passes `observation.json`; Maintain passes the last turn's observation.
     - The actor's own name in the texts uses the same helper. Before, it fell back from participant to id with no observation step.
     - New test "A hosted condense shows a memory told by another NPC under the teller's name". It closes the run, adds a shared copy (toldBy Saadia) through `RunStore::AddActorSummary`, reopens with summary_cap 11, and Hulda's next turn condenses. The condense call holds "# What Hulda remembers\n- (told by Saadia) Secret.\n\nCondense what Hulda".
   - `is_object()` guard replaced by MORPHEUS_REQUIRE, in Pass and in Upkeep.
   - Locals `actors` renamed to `memory`.
   - `HostedScript::Responder` uses `test::IsHostedFold` and a new `test::IsHostedCondense` (the last message ends with HOSTED_CONDENSE_INSTRUCTION's text after its last `{actor}`), not literal prefixes.
   - RunTurn now names the upkeep in a local (`MemoryUpkeep upkeep = Upkeep(...)`) to keep the line within 120 columns.

### TDD evidence

RED (after the tests, before the fixes; `cmake --build --preset tests` was clean):
```
$ morpheus_tests.exe "settings.example.json loads with the current loader"
  CHECK( settings.budget.summaryCap == 1024 )  with expansion: 2048 == 1024
  CHECK( settings.budget.tailAllowance == 3072 ) with expansion: 2048 == 3072
  CHECK( settings.budget.summaryCap < settings.budget.tailAllowance ) with expansion: 2048 < 2048
  test cases: 1 | 1 failed; assertions: 9 | 6 passed | 3 failed
$ morpheus_tests.exe "A hosted condense shows a memory told by another NPC under the teller's name"
  CHECK_THAT( ..., ContainsSubstring("# What Hulda remembers\n- (told by Saadia) Secret.\n\nCondense what Hulda") ) failed
  (the message showed "(told by 0001B07C)"); test cases: 1 | 1 failed
$ timeout 20 morpheus_tests.exe "A hosted NPC's name that holds {actor} fills the hosted texts once"
  exit 124 (hung in the fill loop until the timeout)
$ morpheus_tests.exe "A speaker's L3 holds only the conversation's turns it witnessed"
  All tests passed (6 assertions in 1 test case)   <- coverage for behaviour b697d45 already had
```

GREEN (`cmake --build --preset tests`: no warnings or errors):
```
the four tests above: All tests passed (9 / 6 / 4 / 4 assertions)
[hosted]         All tests passed (942 assertions in 85 test cases)
[context]        All tests passed (236 assertions in 29 test cases)
[turn]           All tests passed (163 assertions in 16 test cases)
[replay]         All tests passed (505 assertions in 7 test cases)
[retrieval]      All tests passed (57 assertions in 11 test cases)
[hosted-server]  All tests passed (1071 assertions in 54 test cases)
[settings]       All tests passed (175 assertions in 12 test cases)
ctest --preset tests: 100% tests passed out of 590
```
(The `[replay]` assertion count varies between runs, 499-507, with the code unchanged: it depends on timing, not on this change.)

### Byte checks

Both greps print 0 0 for settings.example.json, src/turn/HostedSession.h, src/turn/HostedSession.cpp, tests/core/SettingsTests.cpp, tests/hosted/HostedMemoryTests.cpp and tests/hosted/HostedTestSupport.h. The `\n` escapes in the new condense test and in HostedSession.cpp are literal (checked with `grep -F`).

### Concerns

- tests/turn/ReplayTests.cpp line 38 still says its budget is "As settings.example.json" (2048/2048). It is the budget the replays were recorded with, and replay tests must not change, so I left the line and its comment alone. The comment is now stale; if you want it reworded, say so.
- `Upkeep`'s private signature gained `observation` (see 4 above).
- Nothing is left running.
