# Task 10 report: HttpServer, and the fixtures over real HTTP

Status: DONE_WITH_CONCERNS

Commit: 362edc8 HttpServer: the Api over HTTP with Server-Sent Events, stop on disconnect, an exclusive port; the fixtures replayed over HTTP

## What I implemented

- `server::HttpServer` (src/server/HttpServer.h/.cpp), the brief's code: every method (GET, POST, PUT, PATCH, DELETE) on
  `.*` goes to `Api::Handle`; JSON answers are dumped with `error_handler_t::replace`; a turn's stream is written as SSE
  (`event: <name>\ndata: <JSON>\n\n`) from a chunked content provider that polls `TurnStream::Next` every 50 ms, and
  its releaser cancels the turn when the stream ends unsuccessfully (client gone, or the server stopping).
  `SO_EXCLUSIVEADDRUSE` on Windows, `SO_REUSEADDR` elsewhere. `Start(0)` picks a free port; a port that cannot be
  bound throws `core::DataError` naming it.
- src/server/CMakeLists.txt: `HttpServer.cpp` added.
- tests/server/ServerTestSupport.h: `ServerConfigFor`, `Pieces`, `HttpHarness`, `HttpJson`, `JsonPost`, `JsonGet`,
  `HttpTurn`, `PostTurn`, verbatim from the brief, with the new includes.
- tests/server/HttpServerTests.cpp: the brief's seven test cases ([hosted-server][http], three also [fixtures]).

## TDD evidence

RED: `cmake --build --preset tests`
```
D:\MorpheusEngine\tests\server\ServerTestSupport.h(26,1): error C1083: Cannot open include file: 'server/HttpServer.h': No such file or directory
D:\MorpheusEngine\tests\server\HttpServerTests.cpp(15,1): error C1083: Cannot open include file: 'server/HttpServer.h': No such file or directory
```

First run after the implementation (brief's test as written): 6 of 7 passed; the two-runs test failed
(`WaitUntil(folded_through == 1)` false; `completions.size() == 5` was 6, and 8 on other runs). Cause and fix below.

GREEN: `cmake --build --preset tests` clean (no warnings), then
- `./build/tests/bin/Debug/morpheus_tests.exe "[http]"` 13 times in a row: `All tests passed (211 assertions in 7 test cases)` each time (about 2.9 s a run).
- `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` 3 times: `All tests passed (991 assertions in 46 test cases)` each time.
- The two-runs test alone 5 times: `All tests passed (17 assertions in 1 test case)` each time.
- `ctest --preset tests`: `100% tests passed out of 560` (the three [server] real-server tests skipped, as without servers).
- `cmake --build --preset asan` clean, then `./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server]"` 3 times:
  `All tests passed (991 assertions in 46 test cases)` each time, no ASan report.

ASCII checks on all five changed files: `LC_ALL=C grep -c $'[\x80-\xFF]'` printed 0 for each, and the control-byte
grep printed 0 for each. The `\n` escapes in HttpServer.cpp (lines 69-70) and ServerTestSupport.h (line 240) are still
literal backslash sequences. No morpheus_tests or ctest process is left running.

## Deviations from the brief

1. Two-runs test, the fold wait (test change; the flake's cause). The brief polled
   `GET /v1/runs/a/conversations/c1` every 20 ms until `folded_through == 1`. Every request to a run is a job on its
   worker, and `RunWorker::Post` stops a running Maintain (`_maintenanceStop.request_stop()`: "work comes first").
   When the first GET arrived while the fold's completion was streaming, the fold was cancelled. It then ran again and
   got HTTP 500 from the fake, because only one fold reply was queued. Each later GET set `_maintenanceDue` again, so
   folding was retried and failed again. The result was 6 or 8 completions and no fold. Whether the fold finished
   before the first GET was a matter of timing: some runs passed.
   The fix leaves the run alone while it folds and waits on the fakes. It records the embed fake's /embedding count
   before `/end`, then waits until a later /embedding request carries a `search_document: ` content. The folded turn
   is embedded as history in `SyncHistory`, which runs only after `FoldConversation` has stored the fold. After that,
   it makes one GET, checks `folded_through == 1`, and keeps the brief's `completions.size() == 5` and upkeep-slot
   checks unchanged. Nothing was loosened. After the fix, the test passed in every run (listed above).
2. `HttpServer::Start` builds a fresh `httplib::Server` before each bind (the brief's conditional instruction).
   cpp-httplib 0.59's `bind_to_port` sets `is_decommissioned` on failure, and every later `bind_internal` then returns
   -1. Without the fresh server, a second `Start` after a DataError would fail even on a free port. The header comment
   now says Start may be called again after a DataError, and `_pServer`'s comment says each Start replaces it.
3. `HttpServer::Start` calls `_pServer->set_tcp_nodelay(true)`: a fix for a real flaw, with a comment. Each SSE event
   is a small separate `send`. With Nagle's algorithm on (cpp-httplib's default, `CPPHTTPLIB_TCP_NODELAY` false), a
   small write waits until the previous one is acknowledged. A delayed ACK can then stall the token and sentence
   stream for up to about 200 ms an event, and narration latency is what this stream is for. No public interface
   changed.
4. The brief's note on the WaitUntil lambda (a null GET body) no longer applies, since the lambda no longer GETs. The
   one GET after the wait checks `status == 200` with REQUIRE before it reads the body.

## Files changed

- src/server/HttpServer.h (new)
- src/server/HttpServer.cpp (new)
- src/server/CMakeLists.txt
- tests/server/ServerTestSupport.h
- tests/server/HttpServerTests.cpp (new)

## Concerns

- RunWorker design (Task 7, not changed here). Any job, including a read-only GET, cancels a fold that is running.
  The cancelled completion is wasted, and the fold starts over once the queue is empty. A host that polls a run often,
  or keeps a run busy with requests, could keep its folding from ever finishing, and each restart costs a full fold
  prompt on the upkeep slot. Possible remedies are to stop Maintain only for turns, or to let a fold that has started
  finish. That is a design decision for the plan owner.
- When a Maintain failure is caught, `finished` stays true, so `_maintenanceDue` is cleared. Folding is retried only
  after the next job, as the comment there says. This is noted only because it combined with the point above in the
  flaky failure.
- The handlers match `.*` for GET, POST, PUT, PATCH and DELETE only, as in the brief. An OPTIONS request, or a request
  that cpp-httplib rejects before routing, gets cpp-httplib's own answer, without a JSON body or api_version.
