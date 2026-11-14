# D1 Task 2 report: RunStore schema version 3

Status: DONE. Commit 4891ccc "RunStore schema version 3: run kind, hosted tables and methods, in-place upgrade of version 2".

## Implemented
- `src/core/HostedRecord.h` (new): Participant, Conversation, ConversationEvent, Adjudication, HostedTurn, verbatim from the brief.
- `src/store/RunStore.h/.cpp`: RunKind, RunMeta::kind, SCHEMA_VERSION 3, Create (owned only) / CreateHosted / private CreateWith, HOSTED_SCHEMA_SQL, in-place v2 upgrade in Open, kind validation, all hosted methods and RequireHosted, verbatim from the brief.
- `tests/store/HostedStoreTests.cpp` (new, 5 test cases) and the updated version-1 refusal test in `tests/store/RunStoreTests.cpp`.

## TDD evidence
- RED: `cmake --build --preset tests` -> `HostedStoreTests.cpp(15,1): error C1083: Cannot open include file: 'core/HostedRecord.h'`.
- GREEN: `cmake --build --preset tests` (no warnings), `morpheus_tests.exe "[store]"` -> `All tests passed (232 assertions in 29 test cases)`.
- `ctest --preset tests`: `100% tests passed out of 439` (434 + 5), the three `[server]` tests skipped.
- `ctest --preset asan`: `100% tests passed out of 439`.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_play morpheus_probe morpheus_tests`: exit 0, no errors or warnings.

## Byte checks (all five files)
`grep -c $'[\x80-\xFF]'`: 0 for each. `grep -c` control bytes: 0 for each. No backslash escapes were in the new code.

## Files changed
src/core/HostedRecord.h, src/store/RunStore.h, src/store/RunStore.cpp, tests/store/HostedStoreTests.cpp, tests/store/RunStoreTests.cpp.

## Deviations
- RunStore.h/.cpp are CRLF in the working copy; I applied the brief's changes as scripted edits that preserve CRLF instead of overwriting wholesale. Resulting content matches the brief (header text for existing parts kept as before).
- New files were `git add`ed first (commit by path needs them tracked).
- Brief's commit trailer says Opus; I used the attribution given in the session ("Claude Sonnet 5.5").

## Concerns
None. Nothing left running.

## Fix round 1
Fixes per d1-task-2-review.md: Important 1, Minors 1, 2, 3, 4, 6, 7, 8, 9 (5 and 10 left as told).
- Important 1 / Minor 7: HostedTurns and ConversationEvents throw StoreError (file, column) unless every event is an object with a string `text`; observation must be an object; adjudication.params must be an object.
- Minor 1: worldHash comment updated. Minor 2: a failed upgrade is a StoreError naming the file and "cannot upgrade from schema version 2" (rolled back, file stays version 2).
- Minor 3: all JSON columns go through DumpText (replace error handler). Minor 6: FoldConversation requires toTurn > folded_through and <= LatestHostedTurn().
- Minor 8: indexes hosted_turns_conversation and conversation_events_conversation in HOSTED_SCHEMA_SQL (new files and upgrade). Minor 9: RequireOwned guards CommitTurn and TruncateAfter.
- Tests added (HostedStoreTests.cpp, 7 cases): damaged host data, participants, kind, turn columns, event rows (raw SQL damage); failed upgrade stays v2; indexes; fold bounds; kind guards; non-UTF-8 commit.

RED: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"` -> `test cases: 12 | 6 passed | 6 failed`, `assertions: 178 | 150 passed | 28 failed`.
GREEN: same command -> `All tests passed (178 assertions in 12 test cases)`.
Full: `ctest --preset tests` -> 100% passed out of 446 (3 [server] skipped). `ctest --preset asan` -> 100% passed out of 446 (225 s). Dev targets build exit 0, no warnings. Byte greps: 0 and 0 for RunStore.h, RunStore.cpp, HostedStoreTests.cpp.
Note: the first asan run stalled on "LlamaServerBackend keeps one connection ... reconnects after a restart" (unrelated network test, killed); it passes alone under asan and the full asan rerun passed. Likely flaky, not from this change.
No ctest or morpheus_tests process left running.
