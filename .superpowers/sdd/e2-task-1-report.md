# E2 Task 1 report: the host's clock

Commit 4c9f79d "The host's clock: a JSON pointer in the host data, read from every observation".

Implemented per the brief: HostData::clock, Observation::clock, HostedWorld::HasClock, JSON round trip, pointer validation in the constructor, clock fault in Observe, README line.

TDD: RED = `cmake --build --preset tests` -> C2039 'HasClock' / 'clock' is not a member. GREEN = `[hosted]` 1121 assertions in 103 cases passed; `[hosted-server]` passed; `[clock]` 12 assertions in 2 cases passed; `ctest --preset tests`: 100% passed out of 613.

Files: src/turn/HostedWorld.h/.cpp, tests/hosted/HostedWorldTests.cpp, tests/hosted/fixtures/README.md.

Deviations:
- `_clock` is initialised with `nlohmann::json::json_pointer("")` (the pointer has no default constructor; `= {}` did not compile).
- Added `#include <optional>` to the header and the test file.
- The clock key is read before HostDataFromJson's early return for absent lore (otherwise it would be ignored).

Byte checks (both greps) on all four files: 0 and 0. No test processes left running.
Concerns: none.
