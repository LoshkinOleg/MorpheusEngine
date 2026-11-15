# D2 Task 2 report: event overflow cure

Status: DONE. Commit 27137c3.

Implemented per brief: RunTurn fit lambda trims oldest events while tail is over budget with no hits left; record.eventsOmitted set; Entries drops the omitted (oldest) events from a turn's L3 entry; header comments of RecordEvents and RunTurn updated. Old overflow test replaced by the two new tests; added `context/PromptBuilder.h` include.

TDD: RED with `./build/tests/bin/Debug/morpheus_tests.exe "[hosted][turn]"`: first new test failed with TurnError "the observation, the events and the player text use 1429 tokens, over tail_allowance 1024" (12 of 13 passed). GREEN: `"[hosted]"` all passed (790 assertions, 67 cases); `"[turn]"` all passed (151/14); `ctest --preset tests` 100% of 511 (3 [server] skipped).

ASCII/control-byte greps: 0 and 0 for all three files. No test processes left running.

Files: src/turn/HostedSession.cpp, src/turn/HostedSession.h, tests/hosted/HostedTurnTests.cpp.

Deviations: none (include placed alphabetically before core/Errors.h). Concerns: none.

## Fix round

- Entries: a stored events_omitted over the events before its turn now throws store::StoreError ("turn N: events_omitted X is more than the Y events before it"); the clamp is gone. RunStore exposes no file path (its _file is private, only used in its own messages), so the message names the turn and column but not the file; HostedSession holds no path either. Not added, to keep the interface unchanged.
- New test damages events_omitted = 5 through store::Database::Open(file, false) after closing the session, reopens, and checks the next RunTurn throws StoreError containing "events_omitted".
- Overflow test pinned: eventsOmitted == 2 (confirmed), tail lacks a and b, contains c and d.
- RecordEvents comment now says the oldest events (these first, then the request's own) are left out.
- Output: `"[hosted]"` All tests passed (793 assertions, 68 cases); `"[store]"` All tests passed (348 assertions, 41 cases). ASCII/control greps: 0 for all three files.
