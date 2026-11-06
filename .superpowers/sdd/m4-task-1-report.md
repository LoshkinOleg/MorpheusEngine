# M4 Task 1 report: worker events carry the run's memory

Status: DONE

## Implemented
- turn::MemoryView, field `memory` on RunOpened / TurnCompleted / StateChanged, new MemoryUpdated event (non-closing), Session::Notes, Worker::Memory.
- Worker emits MemoryUpdated only when Session::Maintain finished.
- Architecture spec "Process and modules" bullets updated.
- Existing test "PollEvents returns the queued events in order without blocking" fixed exactly as the brief shows. No other test needed filtering.

## TDD evidence
- RED: `cmake --build --preset tests` after adding the test: C2039 'memory': is not a member of 'morpheus::turn::RunOpened'; C2039 'MemoryView': is not a member of 'morpheus::turn'.
- GREEN (intermediate): `[worker]` after implementation: new test passed; only the PollEvents test failed (REQUIRE holds_alternative<TokenStreamed>(events[i]) at WorkerTests.cpp:287), as the brief anticipated.
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[worker]"` x3: "All tests passed (142 assertions in 10 test cases)". `ctest --preset tests`: 100% tests passed out of 200.

## Files changed
src/turn/Session.h, Session.cpp, Worker.h, Worker.cpp; tests/turn/WorkerTests.cpp; docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md.

## Self-review
Matches brief verbatim; comments/layout follow CLAUDE.md (Memory() is a private method after Callbacks; ASCII only). Build had no warnings. tools/play (dev preset only) was not built; it uses get_if chains per the brief.

## Concerns
None.
