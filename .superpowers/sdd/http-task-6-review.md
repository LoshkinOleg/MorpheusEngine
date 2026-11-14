### Spec Compliance
- Spec compliant, with one Important finding the controller's change mandates (below).
  - `/completion` body: every field the brief lists, with the brief's values (LlamaServerBackend.cpp diff lines 380-404). `grammar` goes only on the answer call (460-464). The thinking call has `stop: ["</think>"]` and n_predict = budget minus `close` (420-422). `id_slot` is `spec.slot`, or `_config.slot` when it is -1 (402).
  - Answer prompt after thinking: the first call's ids, then `ServerTokens(thinking, false, false)`, then end + afterEnd, or close (440-456), as the brief requires.
  - The errors match the brief's list: ContractError at 345-347 and 356; InferenceError for no thinking mode (351-355), context overflow (368-372), the count mismatch (407-411), and the stream failures (200-260).
  - Telemetry follows the plan's decisions: 407-415, 430-432, 449, 453, 478-485.
  - Controller additions:
    - Each /completion makes its own client through `MakeClient`, from the stored `_chatHost`/`_chatPort` (160; header diff lines 534-535).
    - `MakeClient` has 3 callers (160, 314, 316). `readUrl` is a local lambda with 2 callers (313, 315).
    - `STREAM_READ_TIMEOUT_SECONDS = 600` is a named constant (56).
    - The `std::stop_callback` stops only that client (161).
    - `Ask` retries once on Read/Write over a socket that was already open (110-116). That covers `/props`, `/tokenize`, `/apply-template` and `/embedding`. It never retries an HTTP status or a parse failure, and `/completion` does not go through `Ask`.
    - RED/GREEN evidence for the retry is in the report.
- Deviation 3 (no retry on `Error::Connection`) is correct. I checked cpp-httplib's `ClientImpl::send_` (build/asan/_deps/morpheus_cpp_httplib-src/httplib.h:15082-15113). A socket found dead is closed and replaced by `ensure_socket_connection`, so `Error::Connection` only ever comes from a fresh connect, never "over a reused socket".
- I checked that a real stale-socket close maps to `Error::Read` and not `ConnectionClosed`. httplib.h:16325-16327 sets `error = Error::Read` whenever the status line or headers cannot be read after a successful write. So the retry catches the real race, not only the fake's headers-then-close drop.
- Cannot verify from diff:
  - Whether llama-server streams the text it holds back as a partial stop-string match (for example a trailing "</thi") when the thinking call ends at "limit". If it does not, `completion.thinking` and the re-tokenized prompt lose that suffix. Only the `[server]` thinking test (Task 10) can confirm this.
  - Greedy behaviour of llama-server's chain at temperature <= 0 (a plan decision, no server here).

### Strengths
- The brief's code is followed faithfully. The one functional addition, `upload_progress` (175), is well reasoned:
  - Checked against httplib.h:15082-15131: the connect runs under `socket_mutex_`, and `socket_requests_in_flight_` only goes up after it.
  - So a `stop()` that runs before `send_` takes that lock finds nothing in flight, closes nothing, and the request would then wait for the first byte.
  - The progress callback (httplib.h:16073-16091) closes that window deterministically.
- Destruction order is right: `closeOnStop` (161) is declared after `pClient` (160), so it is destroyed first and waits for a callback that is still running before the client goes away.
- Exceptions from the receiver, including the caller's `onToken`, are captured as `exception_ptr` and rethrown with their own type after `send` returns (238-247). Nothing is swallowed.
- I verified that the retry drops the failed connection: the `send_` scope_exit calls `disconnect` on `!ret` (httplib.h:15144-15156). The "once only" section (LlamaServerBackendTests.cpp diff 749-755) then shows that the next call works on a new connection.
- The retry tests cover each idempotent path, the once-only limit, no retry on HTTP or parse errors, no retry on a new connection, and no retry of /completion. The fake's `DropNextAnswers` is deterministic and has its own test (FakeLlamaServerTests.cpp diff 688-706).

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
1. **Plan-mandated (controller change): a new TCP connection for every `/completion`, and two for a call that thinks.** This brings back the cost that commit 1c4cad2 removed for every other request. LlamaServerBackend.cpp diff line 160 (`MakeClient` in `StreamCompletion`), called at 424 and 466.
   - The class comment says a new connection costs "about 2 s on Windows when 'localhost' tries ::1 first" (LlamaServerBackend.h diff lines 510-511). llama-server binds 127.0.0.1 by default, and the spec makes localhost hosts the intended use (llama-server-backend-design.md:102).
   - So a `http://localhost:...` chat URL adds about 2 s to every narrator or resolve call on Windows, and about 4 s to one that thinks. That is the hot path.
   - The fast suite cannot show it, because the fakes are reached at 127.0.0.1.
   - Fix while keeping the controller's goal (a cancel never touches the shared client): keep one dedicated member client for streaming, used only by `Complete`, next to `_pChatClient`. `stop()` shuts down only that client's socket, and cpp-httplib reconnects on the next call, so only a cancelled call pays a reconnect.
   - If the owner wants a client per call, resolve the host once. Alternatively, say in the settings docs that `127.0.0.1` must be used rather than `localhost`.
   - This needs the controller's or owner's call, since the dispatch asked for a client per call.

#### Minor (Nice to Have)
1. **The `std::stop_callback` path has no test that fails without it.**
   - In "A stop ends ... mid-stream" (LlamaServerCompleteTests.cpp diff 1194-1228), the stop is requested inside `onPiece`, on the receiver's own thread. The receiver's `stop_requested()` check (189-191) ends the request whether or not `closeOnStop` exists.
   - The held stream pings every 10 ms (FakeLlamaServer.h:76), so even a stop from another thread would be caught by the receiver.
   - The case the callback exists for is a stop from another thread while no bytes arrive (a long prompt read), and it is untested. A `CompletionScript` option that waits silently before its first byte, with the stop requested from a second thread, would cover it.
2. **A stop can block the thread that requests it for as long as a connect takes.** `ClientImpl::stop()` takes `socket_mutex_` (httplib.h:17321-17322), and `send_` holds that mutex while it connects (httplib.h:15083-15113). So a `request_stop()` made during the connect runs `pClient->stop()` on the requesting thread (possibly the UI's) and waits up to `CONNECT_TIMEOUT_SECONDS`, or about 2 s for the localhost case above.
   - The comment at 157-159 ("before there is a connection to close, is seen once the request is written") does not describe this.
   - Important finding 1's member client would also shrink this window.
3. **On a cancel, the telemetry is less complete than in-process.**
   - Here a cancelled call returns `promptTokens`, plus the partial text or thinking, and nothing else (426-428, 474-476).
   - `LlamaCppBackend` fills `tokensEvaluated`/`promptMs` on a cancel during the prompt (LlamaCppBackend.cpp:304-308), `thinkingMs` on a cancel while thinking (358-361), and `tokensGenerated` while answering (402-446).
   - A cancelled answer after thinking also leaves out the answer call's `promptMs` from `thinkingMs`.
4. **The answer call after thinking does not check the server's prompt count** (478-482: `takePromptNumbers` runs only when there was no thinking).
   - The brief's error list ("a server prompt count that differs from /tokenize's") would also apply to the second prompt, which is longer and includes the re-tokenized thinking. A check on `answered.promptTokens == tokens.size()` would close the gap.
   - The code follows the brief verbatim.
5. **A read timeout on a reused connection is retried, so a hung `/tokenize` or `/embedding` takes 2 x 60 s before it fails.** httplib.h:16325-16327 reports `Error::Read`, not `Error::Timeout`, when the status line is not read in time. The comment at 105-109 only describes the stale-socket race.
6. **`FakeLlamaServer::DropNextAnswers(path, 0)` (or a negative count) drops every later request to `path`.** `_drops[path] += 0` creates an entry, and `--drop->second == 0` is never true afterwards (FakeLlamaServer.h diff 609-613, 652-655). A `count >= 1` check would make the misuse loud.
7. **The thinking budget is checked against the generated count, but the answer prompt uses the re-tokenized thinking (368, 441-443).** A canonical split longer than the model's own can push the answer prompt plus `maxTokens` past `ContextSize()` with no check. That is unlikely, but it is unchecked.

### Assessment
**Task quality:** Needs fixes
**Reasoning:** The implementation matches the brief and the controller's additions, with sound error handling and solid retry tests. The client made for each call puts a new TCP connect, about 2 s with a localhost URL on Windows, on every completion. The controller asked for that design, so the owner or controller must decide whether to keep it or use a dedicated member stream client.

## Re-review 1 (fix diff 9a40832..f14da7a)

### Spec Compliance
- Spec compliant with the controller's decisions.
  - (1) `/completion` goes through a dedicated member client: `_pStreamClient`, built once at LlamaServerBackend.cpp diff line 218 with `STREAM_READ_TIMEOUT_SECONDS`. The stop callback calls only that client's `stop()` (135).
  - (2) The retry condition is at 163-168: the socket was reused, `status == 0`, no captured failure, no stop, and Read or Write.
  - (3) The Minors are fixed:
    - Minor 1: a new silent-stream test from another thread (LlamaServerCompleteTests.cpp diff 805-831), with RED shown by removing the callback (10 s).
    - Minor 2: comment only, at 131-134.
    - Minor 4: `checkPromptCount` after both calls (248-253, 275, 310), with a test at 833-852.
    - Minor 5: the comment in `Ask` (78-81).
    - Minor 6: the count check (FakeLlamaServer.h diff 556-558), with a test at FakeLlamaServerTests.cpp diff 668.
  - Minors 3 and 7 are deferred by the controller.
- Important 1: **resolved.** Calls share one kept connection; the test at LlamaServerCompleteTests.cpp diff 765-769 asserts the same `remotePort`. A cancel drops only the stream client's socket. `_chatHost`/`_chatPort` are now constructor locals (216), and `MakeClient` still has 3 callers (217, 218, 220).
- New retry, checked for correctness:
  - A second send never sees a used receiver state. `status == 0` means `response_handler` never ran, and cpp-httplib calls the content receiver only after the headers, so `pending`, `errorBody` and `finished` are still untouched.
  - A stop during the first request makes it fail with Read or Write. `!stop.stop_requested()` then blocks the retry, and the request returns `cancelled`.
  - A stop after that check but before the retry's `send_` takes the socket lock is caught by `upload_progress`, which returns Canceled.
  - The tests cover the retry (765-778), no retry after a piece arrived (781-803), and no retry on a new connection (LlamaServerBackendTests.cpp diff 724-733).
  - The fake now drops without writing a byte, through the header writer that returns -1 (FakeLlamaServer.h diff 467-472, 608-609). That models a stale-socket close faithfully.

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
1. **The retry's premise is wrong about llama-server, in the comment at LlamaServerBackend.cpp diff 158-162 and in the report's "Notes and concerns".**
   - In the pinned source, a streaming `/completion` posts its task, then blocks in `rd.next(req.should_stop)` until the first result (build/dev/_deps/llama-src/tools/server/server-context.cpp:4128-4198). Only then does it set `status = 200` and return, so the headers go out.
   - So "no status" covers the whole prompt evaluation and the first token. The server has started generating before any byte arrives, and "never after any byte (the server may have started generating)" does not separate the two cases.
   - The behaviour is still safe:
     - No piece reached `onPiece`, so nothing is streamed twice.
     - The first task is cancelled once llama-server sees the closed connection (`should_stop`).
     - The re-sent request is the same prompt on the same slot with the same seed, and is mostly served from the cache.
   - One real cost: cpp-httplib reports a read timeout before the status line as `Error::Read` (httplib.h:16325-16327). A prompt read longer than 600 s on a reused connection is therefore sent again, and fails only after about 1200 s. `Ask`'s comment documents this doubling; `StreamCompletion`'s does not.
   - Fix: correct the comment and the report's claim. Optionally, skip the retry when the first attempt took about as long as the read timeout.
2. **The stop-during-connect comment at LlamaServerBackend.cpp diff 131-134 is inaccurate.**
   - It says the stop "then finds no request in flight to stop". But `send_` raises `socket_requests_in_flight_` inside the same locked block as the connect (httplib.h:15083-15131). So a `stop()` that waited for the connect does find the request in flight, shuts the socket down, and the request fails with Write.
   - No retry follows, because the stop was requested, and the call returns `cancelled`.
   - `upload_progress` is only needed for a stop that runs before `send_` takes the lock. The behaviour is correct; only the wording is wrong.
3. **The silent fake writes its headers before going silent; llama-server writes none.** The fake uses `set_chunked_content_provider`, and cpp-httplib writes the status line and headers before calling the provider (FakeLlamaServer.h diff 491-507).
   - So in "A stop from another thread..." the client has `status == 200` before the stop.
   - The test still checks the stop callback (the RED run proves it), but not the case llama-server really produces: a stop with no status at all.
   - Both cases end in the same `!finished && stop` branch, so this is fidelity only.
4. **A comment reflow left a short line in the constructor comment** ("it ignored). Throws InferenceError", LlamaServerBackend.h diff 379).

### Assessment
**Task quality:** Approved
**Reasoning:** Important 1 is resolved with a dedicated, kept-alive stream client. The new `/completion` retry is correct, guarded against stops and partial answers, and tested RED to GREEN. What is left are documentation inaccuracies about when llama-server sends its headers, and the rare doubled read timeout.
