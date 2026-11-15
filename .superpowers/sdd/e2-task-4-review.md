### Spec Compliance
- Spec compliant. `RewindResult` and `Rewind` match the brief (src/turn/HostedSession.h:57-62, :164-169; src/turn/HostedSession.cpp:559-592). `AbandonRollbackForNewRecord` is called in all six writers and never in Maintain (HostedSession.cpp:174, 182, 227, 282, 427, 537; grep finds no call in Maintain at 432-506). The fixture, the runner steps and the README follow the brief. Addition A: RunStore.cpp:1800-1805, with six callers, each right after `RequireHosted`; summary writes, MarkFolded, RememberFold, SetRememberedThrough and MarkShareApplied are not touched. Addition B: tests/store/RollbackStoreTests.cpp:179-262.
- The deviation the implementer reported (open.json unchanged; the runner sets `HostData::clock` from the fixture's top-level `clock`, tests/hosted/HostedTestSupport.h:215-219) matches the brief's Step 2. I accept it.
- Cannot verify from diff: `ctest --preset asan` was not run, although global constraint 17 names it for the full check (the report says so). The HTTP runner does not know the new steps; per the dispatch, that is Task 5's.

### Strengths
- Checks before abandon, traced in every writer:
  - StartConversation: participant checks at 156-169, abandon at 174.
  - RecordEvents: OpenConversation and CheckEvents, then abandon at 182.
  - SetParticipants: every left/joined/empty check at 206-224, abandon at 227.
  - RunTurn: every request check through the narrate_allowance check at 277, abandon at 282, before NextHostedTurn, the reads and CatchUp at 303.
  - EndConversation: OpenConversation, then abandon at 427.
  - ShareMemories: throws at 514-533, abandon at 537.
  
  A refused request therefore leaves the rollback open. Each writer that dates a record with `LatestClock()` reads it after the abandon, so the date comes from the visible state (RecordEvents 186-187, EndConversation 429, ShareMemories AddPendingShare at 552).
- Rewind's net counts hold. Both hide criteria (turn clock > T, conversation start > T) are monotone in T, and the store refusal means nothing new is written while a rollback is open. So the undo's set and the new apply's set are nested on the same base, and `min` subtraction gives the net change. Fixture walk-through:
  - Step 2.5 (no rollback open): apply, hidden 1, rewound true.
  - Step 10.0 (2.5 open): undo restores 1. LatestClock is 4.0, so no apply; restored 1, rewound true.
  - Step 10.0 again (none open): early path skipped, no apply; rewound false.
  - Step 2.5: hidden 1.
  - Turn at 2.6: abandons. NextHostedTurn is 4.
  - Step 10.0: none open; LatestClock is 2.6, so no apply; rewound false, restored 0. Visible turns 1, 2 and 4 make the count 3.
- The full index sync runs after every rewind that reaches the store (HostedSession.cpp:590). `MemoryIndex::SyncEntries` and `SyncSummaries` rebuild through `Replace` (src/turn/Retrieval.cpp:197, 220), so hidden turns and summaries leave the index. I checked this because hidden records left in the index would leak into retrieval. The same-time early return (HostedSession.cpp:569-572) changes nothing, so it correctly skips the sync.
- The fixture's `prompt_absent` at 2.6 is a real check. The turn's speaker, Saadia, would see her shared copy ("gone upstairs") and the after-turn-2 event ("climbs the stairs") as new events if the rollback had not hidden them.
- The store refusal turns a missing abandon into a loud ContractError instead of a silently inexact undo (RunStore.cpp:1800-1805). The RED evidence shows all six refusals failing before the change.
- Addition B's tests check what was asked: the refold's links and vector are gone and the first condense keeps its 2 links (RollbackStoreTests.cpp:244-253); the later conversation and the c1 event at 5.5 are hidden and come back (:255-286).

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
- tests/hosted/HostedRewindTests.cpp:38-67: the test "Every new record while a rollback is open abandons it" exercises only RecordEvents. RunTurn is covered by the fixture. No test calls StartConversation, SetParticipants, EndConversation or ShareMemories while a rollback is open, so the abandon call at HostedSession.cpp:174, 227, 427 or 537 could be deleted with every test still green. No test checks the controller's named contract either: a refused request (for example RecordEvents with an event lacking `text`, or EndConversation on an ended conversation) leaves `OpenRollback()` set. The brief gave this test verbatim, so this is coverage, not a defect. A SECTION per writer plus one refused-request check would close it.
- tests/store/RollbackStoreTests.cpp:271-282: each `MessageMatches(ContainsSubstring("<method>"))` would also match that method's other ContractErrors. For example, `CommitHostedTurn: expected turn ...` (RunStore.cpp:1147) also names the method. Matching "a rollback is open" would pin the refusal's reason.
- src/turn/HostedSession.cpp:282 and 537: the abandon comes before work that can end without a record. A RunTurn cancelled through `stop` after the abandon (303-305, 349-351, 360-362, 379-381) and a ShareMemories whose share is already pending (543-554) both drop the hidden future without writing anything. This is what the brief says ("before the catch-up", "once it passed its checks"), and the report states the share case, so it is a note for the whole-branch review, not a fix here.
- tests/hosted/HostedTestSupport.h:206-207: the re-wrapped doc comment breaks after "commits. prompt_contains, memories contains," and leaves a ragged short line mid-paragraph. Reflow it.
- src/turn/HostedSession.h:164-168: the Rewind doc names only the DataErrors. Rewind calls SyncHistory, which embeds through the backend (Retrieval.cpp:193, 216), so it can also throw what Open documents for embedding failures (InferenceError).

### Assessment
**Task quality:** Approved
**Reasoning:** Rewind, the abandon in all six writers (each after its checks and before its first write, never in Maintain), the store's refusal and the fixture all match the brief, and the counts in every fixture step trace correctly. The remaining items are coverage of four writers' abandon paths and polish.
