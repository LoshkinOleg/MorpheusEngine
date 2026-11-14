# HTTP Task 6 report: LlamaServerBackend::Complete

Status: DONE_WITH_CONCERNS (minor concerns only, listed below)
Commit: 9a40832 "LlamaServerBackend::Complete: token prompts, in-process sampling, thinking in two calls, slots, telemetry, cancel; retry idempotent requests once over a reused connection" (base 1c4cad2)

## What was implemented

- `LlamaServerBackend::Complete`: the brief's code as written, except where the deviations below say otherwise. It sends a streamed `/completion` with the prompt as token ids. The body carries the full sampling set: `samplers` top_k/top_p/temperature, min_keep 1, dynatemp neutral and seed, with every other sampler neutral. It also carries `id_slot` (spec.slot, or the configured slot when it is -1), `cache_prompt` and `stream`, plus `grammar` on the answer call only. A model that thinks makes two calls on one slot. The thinking call has `stop: ["</think>"]` and n_predict = budget minus the closing sentence. The answer call's prompt is the first call's ids, then `/tokenize` of the thinking text (plain text), then the end marker and blank line or the closing sentence. Telemetry comes from `timings`. The contract checks are the same as in-process: ContractError for maxTokens <= 0, a negative thinking budget, one too small to close the block, or a slot below -1. InferenceError for the context overflow, no thinking mode, HTTP errors, a malformed stream, an unknown stop_type, an early end, and a prompt count that does not match `/tokenize`'s.
- `StreamResult` / `StreamCompletion` are in the anonymous namespace, as the brief has them. Each call has its own `httplib::Client`, built from the validated chat host and port, with `STREAM_READ_TIMEOUT_SECONDS = 600`. A `std::stop_callback` calls that client's `stop()`, so the shared clients are never touched.
- Changes the dispatch asked for after 1c4cad2:
  - The constructor's `connect` lambda is split in two. `readUrl` is a local lambda with two callers that parses and validates the URL and returns host and port. `MakeClient(url, host, port, readTimeoutSeconds)` is a helper in the anonymous namespace with three callers: the chat client, the embedding client and each stream. The chat host and port are stored as new private members `_chatHost` and `_chatPort` (header).
  - `Ask` retries once, which covers `/props`, `/tokenize`, `/apply-template` and `/embedding`. The retry happens when the client's socket was open before the request (`Client::is_socket_open()`, so the connection was reused) and the request failed with `httplib::Error::Read` or `Error::Write`. HTTP error statuses, unreadable answers, failures on a new connection and `/completion` are never retried.
- Fake llama-server (`tests/fakes/FakeLlamaServer.h`): `DropNextAnswers(path, count = 1)`. The next `count` requests to `path` are recorded, then get a status line and headers, and the connection closes before the body (a content provider that fails), so the client sees `Error::Read`. Restarting the fake on the same port could not produce a failure: cpp-httplib checks the kept socket (`is_socket_alive`) before sending and reconnects silently once it has seen the close. A real failure only happens when the close races with the request, so the test needed a deterministic drop.

## TDD evidence

RED (Complete), after building with the brief's test file:
```
./build/tests/bin/Debug/morpheus_tests.exe "LlamaServerBackend::Complete*,A stop ends LlamaServerBackend*"
... due to unexpected exception with message:
  LlamaServerBackend::Complete is not implemented yet
... CHECK_THROWS_AS( backend.Complete(test::SERVER_PROMPT, spec, {}, {}), core::ContractError )
due to unexpected exception with message:
  LlamaServerBackend::Complete is not implemented yet
...
test cases: 11 | 11 failed
assertions: 28 | 7 passed | 21 failed
```
GREEN (Complete):
```
./build/tests/bin/Debug/morpheus_tests.exe "LlamaServerBackend::Complete*,A stop ends LlamaServerBackend*"
All tests passed (102 assertions in 11 test cases)
```
(Catch2 v3 takes several test names as one comma-separated argument; the brief's two separate arguments matched nothing.)

RED (retry), with the fake's DropNextAnswers and the new tests in place but no retry in Ask:
```
./build/tests/bin/Debug/morpheus_tests.exe "FakeLlamaServer drops*,LlamaServerBackend sends a request once more*,LlamaServerBackend never sends again*"
LlamaServerBackendTests.cpp(365): FAILED:
  CHECK( backend.CountTokens("hello") == 2 )
  cannot reach http://127.0.0.1:64178/tokenize: Failed to read connection
LlamaServerBackendTests.cpp(366): FAILED:  2 == 3
LlamaServerBackendTests.cpp(371): FAILED: ... cannot reach http://127.0.0.1:64182/apply-template: Failed to read connection
LlamaServerBackendTests.cpp(377): FAILED: ... cannot reach http://127.0.0.1:64187/embedding: Failed to read connection
LlamaServerBackendTests.cpp(384/385): FAILED (once only: 2 == 3, then the read failure)
test cases:  3 |  2 passed | 1 failed
assertions: 25 | 17 passed | 8 failed
```
The fake's own test and the "never sends again" test passed in RED: the fake feature existed, and without a retry nothing is sent twice.

GREEN (retry):
```
All tests passed (25 assertions in 3 test cases)
./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"
All tests passed (313 assertions in 33 test cases)
```
Full suite: `ctest --preset tests` gives `100% tests passed out of 409` (395 + 11 Complete + 2 retry + 1 fake). The build had no warnings.
AddressSanitizer: `cmake --preset asan`, `cmake --build --preset asan` (no warnings), then `ctest --preset asan` gives `100% tests passed out of 409`, with 0 "AddressSanitizer" lines in the output (216 s).
Dev: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe` exits 0. Nothing is left running in the background.

## Files changed

- src/llamaserver/LlamaServerBackend.cpp
- src/llamaserver/LlamaServerBackend.h: the `_chatHost` / `_chatPort` members; the class comment now describes the retry and the per-call stream client
- tests/llamaserver/LlamaServerCompleteTests.cpp (new, the brief's file verbatim)
- tests/llamaserver/LlamaServerBackendTests.cpp: two retry test cases
- tests/llamaserver/FakeLlamaServerTests.cpp: one test case for DropNextAnswers
- tests/fakes/FakeLlamaServer.h: DropNextAnswers

## Byte checks

- `check_bytes` on all six files printed nothing.
- `LC_ALL=C grep -c $'[\x80-\xFF]'` gave 0 for every file. A control-byte grep (`[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]`) also gave 0 for every file.
- The escapes are intact: `pending.find('\n')` and `line.back() == '\r'` are literal backslash sequences (LlamaServerBackend.cpp lines 166 and 172).

## Deviations from the brief

1. The brief's `httplib::Client client(url); SetTimeouts(client);` was replaced by `MakeClient(url, _chatHost, _chatPort, STREAM_READ_TIMEOUT_SECONDS)`, as the dispatch required. 1c4cad2 has no `SetTimeouts`, and a client built from a URL string would skip the validation. `StreamCompletion` therefore also takes `host` and `port`.
2. Added `request.upload_progress = [&stop](...) { return !stop.stop_requested(); }`. `httplib::Client::stop()` only shuts a socket down while a request is in flight. In cpp-httplib, "in flight" starts after the connect, so a stop that arrived during the connect was lost. The request then waited for the server's first piece, which could take up to the whole prompt read. The progress callback runs once the body is written and cancels the request (`Error::Canceled`), which closes that gap. No test covers it, because the window is the length of a connect.
3. The retry covers `Error::Read` and `Error::Write` only, not `Error::Connection`. The dispatch said "a connection or read error over a reused socket". In cpp-httplib, `Error::Connection` comes only from connecting a new socket: once the kept socket is found dead it has already reconnected, so the failure is the server's own and not a stale connection. Retrying would double the wait on a server that is down (each refused connect costs about 2 s on Windows). A stale socket that races with the request fails while writing or reading, which gives Write or Read. If the controller meant any transport error, add `Error::Connection` to the condition in `Ask`.
4. Test filter syntax: see the note under TDD evidence.
5. The commit message adds the retry to the brief's subject, plus one body line.

## Concerns

- `Ask` decides that a connection was reused from `is_socket_open()` before the request. If cpp-httplib then finds that socket dead and reconnects, and the new connection fails on read or write, the request is still retried once. That is harmless for these idempotent requests, but it is not strictly "over a reused socket".
- `DropNextAnswers` simulates the race as headers followed by a lost body. A real stale socket usually fails before any headers arrive. The client takes the same path in both cases (`Error::Read`).
- `tokensGenerated` counts the EOS that llama-server samples (from the plan's decisions). The in-process count does not.
- Several lines in the brief's code and tests exceed 120 columns. They were kept verbatim; the repo has no .clang-format.

## Fix round 1 (commit f14da7a, on 9a40832)

### Changes

1. **Important 1:** `/completion` now goes through a dedicated member client, `_pStreamClient`.
   - The constructor builds it with `MakeClient(chatUrl, chatHost, chatPort, STREAM_READ_TIMEOUT_SECONDS)`. Its connection is kept alive between calls.
   - The `std::stop_callback` calls `_pStreamClient->stop()`, which shuts down only that client's socket. The next call reconnects.
   - `StreamCompletion` now takes `httplib::Client&` in place of host and port.
   - `_chatHost` and `_chatPort` are no longer members: the constructor is now their only user, so they are locals (structured binding), and `<tuple>` is gone.
   - Comments updated: the class comment, the constructor comment, the constants and `StreamCompletion`. The stop-during-connect wording follows review Minor 2: cpp-httplib holds the socket lock while it connects, so the stopping thread waits for the connect (up to the connect timeout), and `upload_progress` then sees the stop.
2. **Retry for `/completion`.**
   - Conditions: the socket was open before the request (it was reused), the error is `Error::Read` or `Error::Write`, `status == 0` (no headers arrived, so nothing reached the receiver), no failure was captured from the receiver, and no stop was requested. When all hold, the request is sent once more.
   - A failure after any byte of answer, a stop, and a failure on a new connection are never retried.
3. **Minor 1:** new test "A stop from another thread ends LlamaServerBackend::Complete while the server sends nothing". It uses a new `CompletionScript::silent` option: the fake sends no piece and no ping, and polls `sink.is_writable()` until the client closes, giving up after 10 s.
4. **Minor 4:** the prompt count is now checked after every call by a `checkPromptCount` lambda, against `tokens` as sent, with the same message as before. So the answer call after thinking is checked against the extended prompt.
5. **Minor 6:** `FakeLlamaServer::DropNextAnswers` throws `std::invalid_argument` for a count below 1.
6. **Minor 5:** the retry comment in `Ask` now says that cpp-httplib reports a read timeout as a read failure, so a request that hangs on a reused connection fails only after two read timeouts.

### Fake changes (tests/fakes/FakeLlamaServer.h)

- `DropNextAnswers` now closes the connection without writing a single byte. The old version sent headers, then broke the body; that would not exercise the "no byte arrived" rule. Record marks the answer with an `X-Fake-Drop` header, and a server header writer returns -1 for it, so cpp-httplib closes the connection without writing anything. That is what a real stale socket looks like; the client gets `Error::Read`.
- New `CompletionScript` options:
  - `dropAfterPieces`: close the connection mid-stream after the pieces.
  - `silent`: described in item 3.

### New and changed tests

LlamaServerCompleteTests.cpp:
- "Complete keeps its connection and sends a request once more when it fails unanswered": two calls share one client port; then a drop on the kept connection is retried on a new port.
- "Complete never sends again a request whose answer had begun": `dropAfterPieces` after the first piece over the kept connection gives one request and an InferenceError, and "The " was streamed.
- "A stop from another thread ends ... while the server sends nothing".
- "Complete checks the server's count of the answer's prompt after thinking".

FakeLlamaServerTests.cpp:
- "FakeLlamaServer can close a stream after its pieces".
- The drop test now also checks the count-of-0 throw.

LlamaServerBackendTests.cpp:
- The "never sends again" test is renamed (it no longer says "or a completion"), and its comment now says why the first completion is not retried: it goes over a new connection.

### TDD evidence

RED, with the new tests and fake options in place on the old implementation (count check and member client absent):
```
./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"
LlamaServerCompleteTests.cpp(441): FAILED:
  CHECK( requests[1].remotePort == requests[0].remotePort )
with expansion:
  65442 (0xffa2) == 65441 (0xffa1)
LlamaServerCompleteTests.cpp(446): FAILED:
  CHECK( backend.Complete(...).text == "yes" )
due to unexpected exception with message:
  cannot reach http://127.0.0.1:65437/completion: Failed to read connection
LlamaServerCompleteTests.cpp(448): FAILED:
  REQUIRE( requests.size() == 4 )
with expansion:
  3 == 4
LlamaServerCompleteTests.cpp(523): FAILED:
  CHECK_THROWS_WITH( backend.Complete(test::SERVER_PROMPT, spec, {}, {}), ContainsSubstring("999") && ContainsSubstring("prompt tokens") )
because no exception was thrown where one was expected:
test cases:  38 |  36 passed | 2 failed
assertions: 332 | 328 passed | 4 failed
```

RED for Minor 1 and Minor 6: the fake's throw had already been written, and the stop callback already existed. So I temporarily removed both (the `std::stop_callback` line, and the throw replaced by a no-op), rebuilt, ran the tests, then restored both files from copies:
```
./build/tests/bin/Debug/morpheus_tests.exe "A stop from another thread*,FakeLlamaServer drops*"
LlamaServerCompleteTests.cpp(497): FAILED:
  CHECK( elapsed < std::chrono::seconds(5) )
with expansion:
  10036962600 ns < 5 s
LlamaServerCompleteTests.cpp(502): FAILED:
  CHECK( chat.ClientClosedStream() )
with expansion:
  false
FakeLlamaServerTests.cpp(261): FAILED:
  CHECK_THROWS_AS( fake.DropNextAnswers("/tokenize", 0), std::invalid_argument )
test cases:  2 | 2 failed
assertions: 12 | 9 passed | 3 failed
```

GREEN:
```
./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"
All tests passed (334 assertions in 38 test cases)
```
- The new stop, retry and no-retry tests were run 15 times in a row: `All tests passed (59 assertions in 7 test cases)` every time.
- `ctest --preset tests`: `100% tests passed out of 414` (409 + 5 new). The build had no warnings.
- `cmake --build --preset asan` built with no warnings. `ctest --preset asan` gave `100% tests passed out of 414`, with 0 AddressSanitizer lines.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe` exited 0.
- Nothing is left running.

Byte checks, on all six files:
- `check_bytes` printed nothing.
- `LC_ALL=C grep -c $'[\x80-\xFF]'` gave 0 for every file, and so did the control-byte grep.
- The escapes are intact: `"\n\n"` in the fake (line 233), and `'\n'` and `'\r'` in the backend.

### Notes and concerns

- Minors 3 and 7 are left as they were, per the controller.
- The `/completion` retry treats "no status yet" as "the server never answered". llama-server writes its response headers as soon as its handler returns, before the prompt is read, so a failure before the headers means the request was never taken up. If a server read the request, started the task, and died before its headers, the retry would send the request a second time on the same slot. That is idempotent for a completion with a fixed seed, apart from the wasted work.
- A stop that comes after a call has finished, while the next call has not started, does not touch the client: the `std::stop_callback` lives only for the duration of one `StreamCompletion`.
