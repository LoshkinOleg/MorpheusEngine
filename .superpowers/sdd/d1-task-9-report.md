# Task 9 report

Implemented: the three fixtures (conversation.json, radiant.json, invalid_requests.json), tests/hosted/HostedFixtureTests.cpp (3 test cases) and tests/llama/LlamaHostedTests.cpp, verbatim from the brief. Commit e503882.

## Evidence (no RED: these pin earlier work)
- First run `./build/tests/bin/Debug/morpheus_tests.exe "[fixtures]"`: `All tests passed (85 assertions in 3 test cases)`, no build warnings.
- Each test shown able to fail (one expectation broken, run, restored):
  - conversation.json: `"As you wish."` -> `"As you wish!"`: FAILED `record->line == expect line` ("As you wish." == "As you wish!").
  - radiant.json: last L3 entry changed: FAILED on the l3 comparison.
  - invalid_requests.json: `'c9'` -> `'c8'` in the error: FAILED on CHECK_THROWS_MATCHES.
  - Restored; rerun passes (85 assertions).
- `ctest --preset tests`: 100% passed out of 499 (496 + 3), 3 [server] skipped.
- `ctest --preset asan`: 100% passed out of 499, same 3 skipped.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`: builds, no warnings (my first call's output tail was cut mid-build; a second call finished it).
- Model-backed (Qwen3-4B + nomic): `All tests passed (16 assertions in 1 test case)`. Model output:
  - Turn 1 "Hulda, I need a room for the night.": action give_item {"item":"key to room 304"}; line "Aela, I'll give you a room for the night--room 304, up the back corridor. Just be careful who you talk to after dark. The wind carries whispers, and not all of them are from the past."
  - Turn 2 "Saadia keeps staring at us. Should I worry?": action give_item {"item":"key to room 304"} (same resolve text printed); line "Aela, don't look so tense. Saadia's just watching the way the wind moves through the hall--she's been here long enough to know when someone's hiding something. But she's not a threat, not tonight."
  - Turn 3 "Then follow me, we are leaving.": action follow {}; line "Aela's already moving, so I'll follow. Don't let Saadia catch you staring--she knows things that don't belong in the wind."
- Byte checks: both greps printed 0 for all five files.
- `tasklist | grep -i -E "morpheus|ctest"`: nothing running.

## Deviations
None: every fixture error message matched the current code. Test count base 496 -> 499.

## Concerns
Model-backed turn 2 repeated give_item (a valid but odd action); the test only checks validity. Git warns LF->CRLF on the new files (autocrlf), harmless.

## Fix round 1
- Added tests/hosted/fixtures/README.md (fixture files, conversation fixture format with every expect key, invalid-requests format, replaceability).
- HostedFixtureTests.cpp: comment why L3 starts at message 1 (message 0 is the system message).
- LlamaHostedTests.cpp: comment naming the budget fields in order.
- Other minors left as instructed.
- Verification: byte checks 0 on all three files (both greps); [fixtures] 85 assertions in 3 test cases passed; ctest --preset tests 100% passed out of 499; tasklist shows nothing running.
