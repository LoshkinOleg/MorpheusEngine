# Final review fixes, sub-project C (base c7e889f)

Status: DONE_WITH_CONCERNS (minor, below)
Commit: bb33676 "Model seat Check reports an unreachable tokenizer instead of ending the app; correct the llama-server stop and retry comments, the spec's turn row and context_size, Complete's grammar contract"

## Findings

1. Important 1, ModelSeat::Check on the UI thread.
   - src/app/model/ModelSeat.cpp:129-141: the `CountTokens` call is wrapped in `try`/`catch (const inference::InferenceError&)`. A failure becomes the problem "cannot count tokens: <what>" (appended after the grammar's verdict with "; " when the grammar also rejects the reply), `tokens` stays 0, and the request stays pending. Only `InferenceError` is caught. The comment says a hung server blocks the check (and the window) for up to the read timeout (60 s) and once more for its one retry. Added includes `<string>` and `inference/Llm.h`.
   - src/app/model/ModelSeat.h:58-59 (SeatCheck field comments) and :97-99 (Check's comment) describe the new contract.
   - Test: tests/app/ModelSeatTests.cpp:39 `UnreachableTokenizerBackend` (FakeBackend whose CountTokens throws InferenceError), test case at :217 "ModelSeat::Check reports a token count that fails as the problem, and the request stays pending": a fitting reply gives exactly the count failure, a non-fitting one gives both, then the request is still pending and Submit answers it.
   - RED: `./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"` ->
     `ModelSeatTests.cpp(233): FAILED: REQUIRE_NOTHROW( checked = seat.Check(published.id, "yes") ) due to unexpected exception with message: cannot reach http://127.0.0.1:8080/tokenize`; `test cases: 18 | 17 passed | 1 failed`.
   - GREEN: same command -> `All tests passed (3249 assertions in 18 test cases)`.
2. src/llamaserver/LlamaServerBackend.cpp:142-147: the stop-during-connect comment now says cpp-httplib marks the request in flight under the same lock it holds while connecting, so a stop that waits out the connect finds the request in flight and shuts its socket down (write fails, retry barred by the stop, call cancelled); upload_progress also catches a stop before the body is written.
3. Retry wording.
   - LlamaServerBackend.cpp:231-241: "reused" is judged by is_socket_open() before the request (a silent cpp-httplib reconnect is then retried too, one extra attempt); llama-server sends the streaming headers only after the first token, so "no status yet" covers the whole prompt read; a failure or read timeout during a long prompt read is retried, so a hung server takes about two read timeouts (about 1200 s); a retried prompt runs again on the same slot, mostly from its cache.
   - LlamaServerBackend.h:38-42: the same, shortened.
4. docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:35: `turn` row dependencies are "core, store, inference, context, grammar".
5. Same spec :134: `n_ctx` -> `context_size`.
6. src/inference/Llm.h:134-139: ContractError for an unparsable grammar is in-process (LlamaCppBackend); LlamaServerBackend reports the server's refusal (HTTP 400) as InferenceError.
7. tests/llamaserver/LlamaServerLiveTests.cpp:204-208: `REQUIRE` -> `CHECK` on the log size, then `continue` for that session when the size is wrong, so the figure loop never indexes a short log and the other sessions' figures still print.
   - RED/GREEN: not demonstrable here. The test is `[server]` and skips without running llama-servers, and the change only shows on a failing live run (a short session). Verified that it compiles under /W4 /WX in tests, asan and dev, and that the three `[server]` tests still skip.
8. README.md:98 (llama-server section): one bullet that the app keeps its backend while settings are unchanged and the backend reads the servers' facts once at startup, so after swapping a server's model under the same URL reload the backend (change and restore a setting, or restart the app), otherwise new embeddings are stored under the old embedding model's id.

## Byte checks (every edited file)

`LC_ALL=C grep -n $'[\x80-\xFF]' <file>` printed nothing and `LC_ALL=C grep -c` for control bytes printed 0 for ModelSeat.cpp, ModelSeat.h, LlamaServerBackend.cpp, LlamaServerBackend.h, Llm.h, ModelSeatTests.cpp, LlamaServerLiveTests.cpp, README.md and the architecture spec. No escape sequences were written.

## Commands and results

- `cmake --build --preset tests` (no warnings), `ctest --preset tests`: 100% tests passed out of 427 (426 base + 1 new), 3 `[server]` skipped.
- `cmake --build --preset asan` (exit 0; only the usual LNK4300 ASAN /INCREMENTAL note), `ctest --preset asan`: 100% tests passed out of 427, 3 `[server]` skipped.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`: exit 0, 0 compiler warnings or errors; all four binaries linked.
- The tests and asan runs and the dev build were started with long timeouts; the harness moved the first run to the background after 180 s, so I ran the later ones as background commands writing to a log and waited for each before starting the next. Nothing is still running.

## Concerns

- When the count fails the window still prints "0 of N answer tokens" above the problem (ModelSeatWindow.cpp:153 prints `tokens` unconditionally). I left the window as it was ("do not change anything else"); the problem line says why. Making `tokens` optional would fix that, but it changes the SeatCheck interface.
- The review also suggested a sentence in the architecture spec's thread paragraph saying Check makes a blocking HTTP request under llama-server. The dispatch did not list it, so it is only in the code comment.
- The spec's Budget bullet (:102) still writes `n_ctx` as a formula term; only :134 (the settings key) was in scope.
