# Task 8 report
Implemented HostedSession::Maintain (+ MAINTENANCE_INSTRUCTION) and tests/hosted/HostedFoldingTests.cpp, verbatim from the brief.
RED: `cmake --build --preset tests` -> C2039 'Maintain': is not a member of 'morpheus::turn::HostedSession' (5 sites).
GREEN: `[hosted]` 593 assertions / 54 cases passed; ctest tests 100% of 496 (493+3), 3 [server] skipped; asan 100% of 496. No warnings (only the usual asan LNK4300).
Byte checks: both greps 0 for all three files.
Deviations: none. Concerns: none.
