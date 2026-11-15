# E1 final fix round: report

Implementer: Opus 5.5. Base 40ca5e5. Brief: `.superpowers/sdd/e1-final-fix-brief.md`; review:
`.superpowers/sdd/e1-final-review.md`.

## Commits

- ebe1d50 Version 4 upgrade by end turns (item 1, I1)
- 0201a76 Memory writes sync only what changed (item 2, I2)
- ce43a0b Background memory upkeep yields between steps (item 3)
- 82431b4 Review minors (item 4)

## 1. The version 4 upgrade gives a memory only to its witnesses (ebe1d50)

What changed (src/store/RunStore.cpp, `moveSummaries` in `RunStore::Open`):
- A version 4 summary's conversation is decided by its end turns (`from_turn`, `to_turn`). The same conversation at
  both ends means a fold of that conversation: the copies go only to the NPC witnesses of that conversation's turns
  inside the range, labelled with it. Different conversations mean a condense: the copies go to the NPC witnesses of
  every turn in the range, with no conversation (as before).
- An end turn that is not a hosted turn, or from_turn > to_turn, is a StoreError naming the file and the summary
  ("summary N covers turns A..B, which do not start and end on hosted turns"). The upgrade rolls back.
- The comment that called the empty label the safe outcome is replaced. It now says why the endpoint rule is safe: a
  D1 condense whose ends fall in one conversation goes to fewer NPCs, so a memory can be lost but never leaked.
- RunStore.h's Open comment and the per-speaker memory spec's upgrade paragraph describe the new rule.

Tests (tests/store/HostedStoreTests.cpp):
- Replaced "A version 4 summary over turns of two conversations ..." with "A version 4 interleaved fold goes only to
  its conversation's witnesses; a condense over two conversations goes to every witness with no conversation". Setup:
  c1 has turns 1 and 3, c2 has turns 2 and 4. The summaries are 1..1 (c1), 1..3 (c1's interleaved fold) and 1..4 (a
  condense). Hulda gets all three, the first two labelled c1. Saadia gets only the condense, with an empty label.
- New: "A version 4 summary whose end turn is not a hosted turn fails the upgrade, naming the file and the summary"
  (1..2 with only turn 1).
- The existing "range holds no turn" test (5..6) still passes under the new message ("summary 1").

RED (new interleaved test, before the fix):
```
HostedStoreTests.cpp(873): FAILED:  CHECK( hulda[1].conversationId == "c1" )  with expansion:  "" == "c1"
HostedStoreTests.cpp(880): FAILED:  REQUIRE( saadia.size() == 1 )  with expansion:  2 == 1
test cases:  1 | 1 failed
```
The end-turn test was written in the same edit as the fix, so it has no recorded RED. Before the fix that row upgraded
silently, because turn 1 lies in its range.

GREEN: `"[store]"`: All tests passed (434 assertions in 50 test cases); `"[hosted]"`: 1055 assertions in 95 test cases;
`ctest --preset tests`: 100% of 604 passed.

## 2. Memory writes sync only what changed (0201a76)

What changed (src/turn/HostedSession.h/.cpp):
- New private `SyncMemories()`, which only calls `_index.SyncSummaries(_store.HostedSummaries(), ...)`. It has four
  callers: SyncHistory, a catch-up's storeFold, storeCondensed and ApplyShare.
- `SyncHistory()` (the turn index, then SyncMemories) now runs only when a conversation's folded_through moved:
  `Open`, a fold during a turn (storeFold with foldsConversation) and `Maintain`'s MarkFolded.
- A catch-up fold, a condense and an applied share call only SyncMemories.
- `Entries` takes the conversation's turns (a new parameter on this private method), so each conversation's turns are
  read once per sync. SyncHistory reads them once for the witness map and the entries. CatchUp passes the turns it
  already read. RunTurn passes `played` (no turn is committed during the fit loop). There is no behaviour change.

Test (tests/hosted/HostedMemoryTests.cpp): "A fold during a turn indexes its turns; a share and a catch-up with its
condense index only memories".
- Embedded vectors are reused by content hash, so `EmbeddedTexts()` alone cannot tell a re-sync from no sync. The
  test therefore deletes the stored turn vectors (`DELETE FROM embeddings WHERE kind = 'turn'`, through a second
  connection) before each memory-only write. After the write it checks two things:
  - no turn vector came back (`turnVectors() == 0`);
  - `EmbeddedTexts()` grew by exactly the new summaries: 1 for the share, Saadia's new memories for the catch-up with
    its condense.
- A fold during Hulda's turn adds exactly the newly folded turns' vectors (`turnVectors() == 1 + folded`).

RED (before the change):
```
HostedMemoryTests.cpp(532): FAILED:  CHECK( backend.EmbeddedTexts() == embedded + 1 )   14 == 12
HostedMemoryTests.cpp(533): FAILED:  CHECK( turnVectors() == 0 )   2 == 0
HostedMemoryTests.cpp(543): FAILED:  CHECK( backend.EmbeddedTexts() == embedded + static_cast<int>(saadia.size()) - 1 )   19 == 17
HostedMemoryTests.cpp(544): FAILED:  CHECK( turnVectors() == 0 )   2 == 0
```
GREEN: the test passes (21 assertions). `[hosted]` 96 cases, `[hosted-server]` 57, `[turn]` 16, `[replay]` 7 and
`[context]` 29 all pass. `ctest --preset tests`: 100% of 605 passed.

Left for later, as the brief says:
- An incremental index: append new turn and summary entries instead of SyncEntries/SyncSummaries reloading every
  stored vector of the kind and every summary row.
- A cheaper Maintain check: skip a conversation whose NPC marks all reach its last turn, with an SQL check that does
  not parse turns. Today Maintain still calls HostedTurns and NpcWitnesses for every conversation on every pass.

## 3. Background folding stops between catch-ups when any request waits (ce43a0b)

What changed:
- `HostedSession::Maintain(std::stop_token stop, std::stop_token yield = {})`.
  - The pass is made of steps: an ended conversation's MarkFolded with its SyncHistory, one NPC's catch-up in one
    conversation, and one applied pending share.
  - Before each step that has work, it returns false when `yield` is requested and this pass already finished a step.
    A step in progress finishes, and every pass makes progress however often requests arrive.
  - An NPC already caught up is not a step and is skipped without a check.
  - `stop` works as before.
- `RunWorker`: a new `_maintenanceYield` stop source, fresh for each pass, is handed to Maintain. `Enqueue` requests
  it for every queued job; a preempting job (Preempt::Fold) also requests the stop, as before.
- Comments updated: the RunWorker class comment, Preempt, Post, Enqueue, the worker loop and Reopen; the HostedSession
  class contract and the Maintain comment.
- Docs updated:
  - the morpheus_server spec, section 2 "One worker per run". The old text said Maintain was not split per
    conversation; it now describes the yield.
  - the per-speaker memory spec's Maintain bullet.
  - README "A run folds its ended conversations in the background ...".
  - a CHANGELOG 1.1.0 bullet.

Deviation within the brief: "Maintain returns false when yield is requested" is honoured only after the pass finished
a step of its own. This guarantees progress: if a host polls often, a yield could otherwise arrive before every pass
did anything, and folding would never move. Each pass now does at least one step. So a pass asked to yield before it
starts still does one step (tested). A yield asked after the last step changes nothing: the pass returns true.

Tests:
- tests/hosted/HostedMemoryTests.cpp:
  - "Maintain asked to yield returns between NPC catch-ups and between conversations; the next pass resumes". Every
    fold call requests the yield. Pass 1 makes one fold, so Hulda remembers and Saadia does not. Pass 2 makes one fold
    (Saadia) and yields before the second conversation, whose folded_through stays 0. Pass 3 finishes (true), and a
    further pass makes no fold.
  - "Maintain asked to yield applies one pending share per pass; the next pass applies the rest": two complete pending
    shares and a yield already requested. Pass 1 applies one share and returns false; pass 2 applies the other.
- tests/server/RunWorkerTests.cpp: "A job that does not preempt waits only for the running catch-up, not the whole
  pass, which completes later".
  - An ended conversation that Hulda and Saadia witnessed has two catch-up folds, each about 900 ms.
  - A job queued during the first one runs when exactly one fold call was made and one memory exists.
  - The pass then completes: both remember, with exactly 2 fold calls, so nothing was started over.

RED, HostedSession (with the new signature stubbed so the tests compile and the yield is ignored):
```
HostedMemoryTests.cpp(576): FAILED:  CHECK_FALSE( session.Maintain({}, yield.get_token()) )  !true
HostedMemoryTests.cpp(577): FAILED:  CHECK( folds == 1 )  3 == 1
HostedMemoryTests.cpp(586): FAILED:  CHECK( session.Conversation(second)->foldedThrough == 0 )  2 == 0
HostedMemoryTests.cpp(627): FAILED:  CHECK_FALSE( session.Maintain({}, yield.get_token()) )  !true
... test cases: 2 | 0 passed | 2 failed; assertions: 23 | 14 passed | 9 failed
```
(Before the stub, it failed to compile: C2660 Maintain does not take 2 arguments.)

RED, RunWorker:
```
RunWorkerTests.cpp(384): FAILED:  CHECK( foldsBeforeJob == 1 )  2 == 1
RunWorkerTests.cpp(385): FAILED:  CHECK( memories == 1 )  2 == 1
```
GREEN:
- `"Maintain asked to yield*"`: 23 assertions in 2 test cases.
- `"[worker]"` 5 runs: all passed, 21 cases each.
- The new worker test 10 runs in a row: all passed.
- The existing tests are unchanged and pass: "A job that does not preempt waits for a running fold, which lands first"
  and "A preempting job stops a running fold at once ...".
- `ctest --preset tests`: 100% of 608 passed.
- The asan repeat runs are listed under "Finally".

## 4. Small items from the review's triage (82431b4)

The review's triage has no separate list for "fix now". Its "Must fix" is I1 (item 1). Its "Should fix with I2" items
are both in item 2:
- "SyncHistory reads HostedTurns twice": done.
- "Maintain reads every turn each pass": the brief defers it.

I also took the review's Minors that are not on its "Can wait" list, are not spec or owner decisions, and are small:

- Minor 2: the v4 upgrade silently skipped a non-string listener id. It is now a StoreError ("hosted turn N cannot be
  upgraded: its listener_ids or observation is damaged"), as every normal read throws.
  - Test: "A version 4 hosted turn whose listener id is not a string fails the upgrade, naming the file and the
    column".
  - RED: no exception was thrown (2 failed). GREEN: passes.
- Minor 1 (spec deviation): "a mark of an unknown conversation" is now the spec's StoreError. A hosted `RunStore::Open`
  checks `actor_memory` for a conversation_id that is not in `conversations` and names the file, the column
  (`actor_memory.conversation_id`) and the id. RunStore.h's Open comment says so.
  - Test: "A hosted run with a memory mark of a conversation it does not have is a StoreError naming the file and the
    column".
  - RED: no exception was thrown (2 failed). GREEN: passes.
- Minor 4: a retrieved memory's `timings.retrieval[].source` is "memory" (it was "history"). CHANGELOG 1.1.0 notes it.
  - Test: "A hosted turn's retrieval timings name a retrieved memory 'memory' and a retrieved turn 'history'". It
    checks (source, ref) pairs, because a summary id and a turn number can both be "1".
  - RED: `CHECK( hits.contains({"memory", ...}) )` false. GREEN: passes.

Skipped:
- Minor 3, the pending-share StoreError names the row, not the file: the review lists it under "Can wait" (Task 6). It
  needs a RunStore path accessor.
- Minor 5, retrieval repeating shown memories: a spec change, the owner's call.
- Minor 6, reusing vectors by content hash across copies: an optimisation that matters only at upgrade time or for
  large shares.
- Minors 7 and 8: spec-level, noted for F/G.
- Minor 9, Maintain with stop already requested still marks one ended conversation: "Can wait" (Task 5). Item 3 checks
  only the yield before MarkFolded, not the stop.
- The whole "Can wait" list: untested guards, ModelSeatTests comment, MemoryUpkeep comment reflow, `<map>` include
  order, the arch spec's "any other version is refused", the HTTP fixture waits, the unused `<vector>`, the ReplayTests
  comment width, and the duplicated LlamaHostedTests overrides.

## Finally

- `ctest --preset tests`: 100% tests passed out of 611 (5 [server]/llama-server tests skipped as always).
- asan: `cmake --build --preset asan` clean (exit 0, no warnings); `ctest --preset asan`: 100% tests passed out of 611.
  Under asan, `"[worker]"` 5 runs all passed (21 cases each) and `"Maintain asked to yield*"` 3 runs all passed.
- Morpheus's own game: `"[turn]"` 16 cases, `"[replay]"` 7, `"[context]"` 29, all passed after items 2 and 3. No
  Session test, replay fixture or owned prompt was touched. Item 4 changed only RunStore's hosted paths and a hosted
  timings label.
- Byte checks: every changed file printed 0 for both greps (`[\x80-\xFF]` and the control-byte class). Files:
  - RunStore.cpp/.h, HostedSession.cpp/.h, RunWorker.cpp/.h;
  - HostedStoreTests.cpp, HostedMemoryTests.cpp, RunWorkerTests.cpp;
  - CHANGELOG.md, README.md, both specs.
  No escape sequences were written except the test's `\"` in a C++ literal, which grep shows as written.

## Files changed

- src/store/RunStore.cpp, src/store/RunStore.h
- src/turn/HostedSession.cpp, src/turn/HostedSession.h
- src/server/RunWorker.cpp, src/server/RunWorker.h
- tests/store/HostedStoreTests.cpp, tests/hosted/HostedMemoryTests.cpp, tests/server/RunWorkerTests.cpp
- docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md, docs/superpowers/specs/2026-11-16-morpheus-server-design.md
- README.md, CHANGELOG.md

## Concerns

- Item 3 yields only after a step of its own (progress guarantee, described above). If the owner wants a yield
  requested before a pass to return at once, it is a one-line change, but a polling host could then starve folding.
- `.superpowers/sdd/progress.md` (the ledger) still carries Task 1's "acceptable, safer" note. I did not touch it:
  it is the controller's file and never staged.
- The memory mark check adds one small SQL query to every hosted Open.
- The hosted-world (D1) spec's description of `Maintain(std::stop_token stop)` is historical and was left as it is.
