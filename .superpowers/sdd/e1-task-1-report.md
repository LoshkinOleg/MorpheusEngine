# E1 Task 1 report: store version 5

Status: DONE_WITH_CONCERNS

Commit: 02750d8 "Store version 5: summaries per actor, remembered-through marks and pending shares; version 4 hosted summaries move to their NPC witnesses"

## What was implemented

- `core::Summary` gains `actorId`, `conversationId`, `toldBy`, `sourceId`, `superseded`, `id` after `text` (`<cstdint>` added). `core::PendingShare` added to HostedRecord.h after `ConversationEvent`.
- `store::EmbeddingKind::Summary` (stored as "summary"; `KindName` handles it; there is no kind parser). `RunStore::SCHEMA_VERSION == 5`.
- Schema: `summaries` in `SCHEMA_SQL` gains the five columns. `PER_ACTOR_COLUMNS_SQL` (the ALTER TABLE lines, upgrades only) and `PER_ACTOR_TABLES_SQL` (`actor_memory`, `pending_shares`, index `summaries_actor`; new files and upgrades). `HOSTED_SCHEMA_SQL` is unchanged (it never held the new tables).
- `RunStore::Open` upgrade chain in one transaction: v2 runs `HOSTED_SCHEMA_SQL` and inserts kind=owned; v3 runs `EVENTS_OMITTED_SQL`; v2/v3/v4 then run both per-actor SQL constants; a hosted run (kind read from run_meta; v2 is owned) then moves its run-wide summaries with the brief's `moveSummaries` code, changed only to read the summary rows (and the conversations) into memory before inserting. Error message "cannot upgrade from schema version {old} to 5" kept.
- New hosted-only methods (ContractError on an owned run): `ActorSummaries`, `HostedSummaries`, `AddActorSummary`, `RememberFold`, `CondenseActorSummaries`, `RememberedThrough`, `SetRememberedThrough`, `MarkFolded`, `AddPendingShare`, `PendingShares`, `RemovePendingShare`. `FoldConversation` returns the new id, requires a non-empty actor and `conversationId == id`, and moves the actor's mark too, all in one transaction.
- `AddSummary`, `ReplaceSummaries`, `Summaries` call `RequireOwned` and touch only rows with `actor_id = ''`.
- Private helpers: `ReadSummaryRow` (ActorSummaries, HostedSummaries) as in the brief; `MoveFoldedThrough` (FoldConversation, MarkFolded: checks the conversation exists, the mark moves forward and not past LatestHostedTurn, then updates). The shared insert is the public `AddActorSummary` (it opens no transaction, so FoldConversation, RememberFold and CondenseActorSummaries call it inside theirs); ids come from `SELECT last_insert_rowid()` (Database has no method for it).
- Step 6: `HostedSession::Upkeep(conversationId, speakerId, turn)` and `Pass(conversationId, speakerId, hits, inputs)`; the fold sets `actorId` = speaker and `conversationId`; `Pass` reads `ActorSummaries(speakerId, false)`; `Maintain` folds an ended conversation as its last turn's speaker; `Summaries()` became `Memories(actorId)` (`ActorSummaries(actorId, true)`).

## TDD evidence

RED: `cmake --build --preset tests`
```
D:\MorpheusEngine\tests\hosted\HostedFoldingTests.cpp(135,58): error C2039: 'Memories': is not a member of 'morpheus::turn::HostedSession'
D:\MorpheusEngine\tests\store\HostedStoreTests.cpp(119,13): error C2039: 'actorId': is not a member of 'morpheus::core::Summary'
```

GREEN: `cmake --build --preset tests` (no errors, no warnings), then:
```
"[store]"   All tests passed (411 assertions in 47 test cases)
"[hosted]"  All tests passed (859 assertions in 74 test cases)
"[turn]"    All tests passed (154 assertions in 15 test cases)
"[replay]"  All tests passed (501 assertions in 7 test cases)
"[context]" All tests passed (224 assertions in 27 test cases)
ctest --preset tests: 100% tests passed out of 577 (4 [server]/llama-server tests skipped, as usual)
```
The asan preset was not run.

## Files changed

src/core/TurnRecord.h, src/core/HostedRecord.h, src/store/RunStore.h, src/store/RunStore.cpp, src/turn/HostedSession.h, src/turn/HostedSession.cpp, src/turn/Session.cpp, tests/store/HostedStoreTests.cpp, tests/store/RunStoreTests.cpp, tests/hosted/HostedFoldingTests.cpp, tests/hosted/HostedSessionTests.cpp.

ASCII checks (`grep -c $'[\x80-\xFF]'` and the control-byte grep) print 0 for every changed file. No morpheus/ctest process left running.

## Deviations from the brief

1. Test actor ids. `TestHostedTurn` in HostedStoreTests.cpp uses "npc-1" (Hulda, speaker) and "player" (Aela, listener, the player), not the Skyrim ids. The new store tests use "npc-1" for Hulda, "player" for Aela and "npc-2" for Saadia (as the file's other tests do). The rule is the same: the NPC witness gets the summary, the player none.
2. `ActorSummary` helper is in the file's first anonymous namespace (not appended at the end) so the existing fold tests can use it.
3. SQLite refuses to drop an indexed column, so the brief's "make a version 4 file" SQL first needs `DROP INDEX summaries_actor`. It is a shared test constant `DROP_VERSION_5_SQL` used by the v3, v4 hosted and v4 owned upgrade tests (the v3 upgrade test now drops version 5's additions too and expects "5").
4. Existing tests updated to the new contract: the two FoldConversation tests use actor summaries and `ActorSummaries`/`HostedSummaries`; the v2 upgrade test title and expected version are 5; the kind-guard test also checks a few hosted-only memory methods on an owned run; the guard test also checks `ReplaceSummaries` on a hosted run. RunStoreTests: `SCHEMA_VERSION == 5`, "supported version 5".
5. HostedFoldingTests adapted to per-speaker folds: in "Two open conversations interleave...", c1's summary is Hulda's (last speaker of c1), so Saadia's next turn in c2 no longer shows "# Story so far" (the test now checks it is absent and that Saadia has no memories); in "A stop between two ended conversations...", c1's fold is Hulda's and c2's is Saadia's (one each) instead of two run-wide summaries. Other reads use `Memories(test::HULDA)`.
6. src/turn/Session.cpp (not in the brief's file list, committed): the owned game's `kindText` switch over `EmbeddingKind` gets `case Summary: break;` falling to its existing "unhandled" throw, so Linux `-Wswitch` stays clean. No behaviour change.
7. `AddPendingShare` does not check the conversation exists (the brief does not ask); it requires the two actors and the conversation id non-empty. `RemovePendingShare` of an unknown id is a no-op.

## Concerns

1. Hosted condensing between Task 1 and Task 2: `MemoryUpkeep::Condense` still calls `RunStore::Summaries()` / `ReplaceSummaries`, which now throw ContractError on a hosted run. No current test triggers a hosted condense (all suites pass), and Task 2's `SummaryScope` replaces these calls, but until then a hosted run whose speaker's summaries go over summary_cap would fail with ContractError.
2. The v4 move (brief's code, kept as written): a summary's witnesses are every NPC of every turn in [fromTurn, toTurn], and its conversation is the last such turn's. Turn numbers are run-wide, so when conversations interleave, an NPC of another conversation whose turn falls inside the range also gets the copy, labelled with the other conversation's id. A summary whose range holds no turn is removed without a copy. Restricting to the conversation of the summary's toTurn would be more exact for plain folds but wrong for D1's condensed run-wide summaries; I did not change the planned rule.
3. `FoldConversation` also requires the speaker's own mark to move forward (it goes through `SetRememberedThrough`). If a later task lets an NPC's catch-up mark get ahead of the conversation's folded_through before that NPC folds as speaker, that fold would be a ContractError.

## Fix round

Asked by the review of 02750d8: two fixes to the version 4 move in `RunStore::Open`, and the Maintain comment re-wrapped.

1. A version 4 summary whose range holds turns of more than one conversation (a D1 run-wide condense) is now copied to each NPC witness with an empty conversationId, because a condensed summary has no conversation. A summary over one conversation's turns keeps that conversation's id. The move now collects the set of conversation ids in the range.
2. A version 4 summary whose range holds no turn is now a StoreError, `'<file>': summary <id> covers turns <from>..<to>, which hold no hosted turn`, wrapped by Open's "cannot upgrade from schema version 4 to 5". The transaction rolls back and the file stays at version 4 with its summary.
3. src/turn/HostedSession.h: the Maintain comment is re-wrapped to 120 columns. RunStore.h's Open comment now states both rules.

Tests, written first and appended to tests/store/HostedStoreTests.cpp:
- "A version 4 summary over turns of two conversations is upgraded to each NPC witness with no conversation". c1 (Hulda) has turns 1 and 3; c2 (Saadia) has turn 2. One summary covers 1-3 and another covers 1-1. Hulda gets both: "c1" on the 1-1 copy and an empty label on the 1-3 copy. Saadia gets the 1-3 copy with an empty label. The player gets none. Marks: c1/npc-1 = 3, c2/npc-2 = 2.
- "A version 4 summary whose range holds no turn fails the upgrade, naming the file and the summary". It checks the StoreError mentions run.db, "summary 1" and "version 4", and that afterwards the file is still version 4 with one summary row.

RED: `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
```
HostedStoreTests.cpp(885): FAILED: CHECK_THROWS_AS( store::RunStore::Open(file), store::StoreError ) because no exception was thrown
HostedStoreTests.cpp(893): FAILED: CHECK( version.ColumnText(0) == "4" ) with expansion: "5" == "4"
HostedStoreTests.cpp(896): FAILED: CHECK( summaries.ColumnInt(0) == 1 ) with expansion: 0 == 1
HostedStoreTests.cpp(866): FAILED: CHECK( hulda[1].conversationId.empty() ) with expansion: false
HostedStoreTests.cpp(871): FAILED: CHECK( saadia[0].conversationId.empty() ) with expansion: false
test cases: 49 | 47 passed | 2 failed
assertions: 428 | 422 passed | 6 failed
```

GREEN: `cmake --build --preset tests` built with no errors and no warnings. Then:
```
./build/tests/bin/Debug/morpheus_tests.exe "[store]"   All tests passed (428 assertions in 49 test cases)
./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"  All tests passed (876 assertions in 76 test cases)
```
After that run, one comment in RunStore.cpp was shortened to fit 120 columns. Nothing else changed.

ASCII and control-byte greps print 0 for RunStore.cpp, RunStore.h, HostedSession.h and HostedStoreTests.cpp.

Concern 2 of the first round (interleaved witnesses) is narrowed: a copy no longer gets a wrong conversation label. Each NPC of any turn in the range still gets the copy, as the review asked.
