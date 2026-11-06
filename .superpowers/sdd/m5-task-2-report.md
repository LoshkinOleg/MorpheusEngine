# M5 Task 2 report
Implemented TurnRecord::digest, turns.digest column, SCHEMA_VERSION 2, CommitTurn/Turns read+write, comment updates, per the brief verbatim.
RED: `cmake --build --preset tests` -> C2039 'digest': is not a member of TurnRecord.
GREEN: `morpheus_tests.exe "[store]"` -> All tests passed (133 assertions in 24 test cases); `ctest --preset tests` -> 100% passed out of 251.
Files: src/core/TurnRecord.h, src/store/RunStore.h, src/store/RunStore.cpp, tests/store/RunStoreTests.cpp.
Deviations: none. The test file had mixed line endings; normalized to CRLF like the rest of the repo.
Concerns: none.
