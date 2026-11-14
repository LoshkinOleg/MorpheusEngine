# Task 5 report: LlamaServerBackend (startup, prompt tokens, embeddings, state)

Status: DONE_WITH_CONCERNS (concerns are environmental/informational only; see below)

Commit: ff92701 "Add morpheus_llamaserver: startup, prompt tokens with contents as plain text, embeddings; share the content markers" (base 82a1ea4).

## What was implemented

- `inference` (src/inference/Llm.h, Llm.cpp): the thinking texts `THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT`, `CLOSE_THINKING_TEXT` (std::string_view constants), and the shared content-marker helpers `ContentMarker`, `MarkContents`, `TemplatePieces` (with the brief's exact error texts). `CONTENT_MARKER_BYTE` moved here from LlamaCppBackend.cpp.
- `LlamaCppBackend`: `CLOSE_THINKING_TEXT` member removed; the thinking markers are tokenized from the `inference::` texts; `TokenizePrompt` rewritten on `ContentMarker` / `MarkContents` / `TemplatePieces` (same marker, same pieces, same error texts, same BOS de-duplication). Header comments updated as the brief says.
- New `src/llamaserver` (library `morpheus_llamaserver`, alias `Morpheus::llamaserver`): `LlamaServerConfig` and `LlamaServerBackend` exactly as the brief's code: URL validation, `/props` on both servers, the per-slot context check, thinking-switch detection with markers tokenized by the server, BOS detection (empty text with add_special), `CountTokens`/`CountPromptTokens` via `/apply-template` + per-piece `/tokenize` (pieces with parse_special, contents without), a template-opened `<think>\n` dropped, embeddings via `/embedding` with prefixes and L2 normalisation (one pooled vector required, zero vector refused), `SaveState`/`RestoreState` refused, `Complete` a stub throwing `InferenceError("LlamaServerBackend::Complete is not implemented yet")`.
- Root CMakeLists.txt adds `src/llamaserver` under `MORPHEUS_BUILD_LLAMA_SERVER`; tests link `morpheus_llamaserver`.
- Tests: 2 new `[inference]` cases in tests/inference/LlmTests.cpp; tests/llamaserver/ServerTestSupport.h; 10 `[llamaserver]` cases in tests/llamaserver/LlamaServerBackendTests.cpp (brief's code verbatim).
- Docs/consumer check (asked for in the dispatch, not the brief): CLAUDE.md (engine-library list gains `Morpheus::llamaserver`; layout gains a `src/llamaserver` line, wording taken from plan Task 11 step 4.5), README.md (vendored engine-library list; layout line from plan Task 11), cmake/consumer-check/CMakeLists.txt (comment says seven engine libraries; new `Morpheus::llamaserver` FATAL_ERROR check, as plan Task 11 step 1).

## TDD evidence

RED (after writing the tests and the tests/CMakeLists.txt link change, before any implementation):

```
$ cmake --build --preset tests
D:\MorpheusEngine\tests\inference\LlmTests.cpp(63,5): error C3861: 'ContentMarker': identifier not found
D:\MorpheusEngine\tests\inference\LlmTests.cpp(65,5): error C3861: 'ContentMarker': identifier not found
D:\MorpheusEngine\tests\inference\LlmTests.cpp(67,5): error C3861: 'ContentMarker': identifier not found
D:\MorpheusEngine\tests\inference\LlmTests.cpp(69,27): error C3861: 'MarkContents': identifier not found
D:\MorpheusEngine\tests\inference\LlmTests.cpp(79,5): error C3861: 'TemplatePieces': identifier not found
... (TemplatePieces, lines 80-84)
D:\MorpheusEngine\tests\inference\LlmTests.cpp(83,5): error C3861: 'CHECK_THROWS_WITH': identifier not found
D:\MorpheusEngine\tests\inference\LlmTests.cpp(84,5): error C3861: 'CHECK_THROWS_WITH': identifier not found
D:\MorpheusEngine\tests\llamaserver\LlamaServerBackendTests.cpp(14,1): error C1083: Cannot open include file: 'llamaserver/LlamaServerBackend.h': No such file or directory
```

As the brief expected, plus the `CHECK_THROWS_WITH` error (see deviations).

GREEN:

```
$ cmake --build --preset tests            -> no warnings, no errors
$ ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[inference]"
All tests passed (99 assertions in 16 test cases)      (about 8 s; the unreachable-port sections dominate)
$ ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]" --list-tests  -> 10 matching test cases
$ ctest --preset tests
100% tests passed out of 391
```

391 = 379 at base + 12 new (the brief's "389" counted from the plan's older baseline).

Dev build and model-backed tests:

```
$ cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe
exit 0; no warnings or errors outside _deps (morpheus, morpheus_tests, morpheus_play, morpheus_probe, morpheus_llama, morpheus_llamaserver built)

$ MORPHEUS_TEST_CHAT_MODEL=.../Qwen3-4B-Instruct-2507-Q4_K_M.gguf MORPHEUS_TEST_EMBED_MODEL=.../nomic-embed-text-v1.5.Q8_0.gguf \
  ./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"
test cases: 32 | 27 passed | 5 skipped
assertions: 5000 | 5000 passed
```

Skips: the 4 thinking tests that were not run (MORPHEUS_TEST_THINKING_MODEL unset), plus LlamaEmbedStateTests.cpp(216) "cannot hard-link the models into the temp folder: unknown error" (environmental, unrelated to this task).

Thinking check (models/Qwen3.5-9B-Q4_K_M.gguf as MORPHEUS_TEST_THINKING_MODEL): running the whole `[llama]` set in one process, the 4 thinking tests failed with "cannot load model" (ggml_vulkan: device memory allocation of 876 MB failed, on the 8 GB RTX 3070 Laptop after the earlier tests). Run one per process, all five thinking tests pass:
- "A thinking block that runs out of budget is closed by the backend": All tests passed (3 assertions)
- "A thinking budget too small to close the block is a contract error": passed (1)
- "A thinking model thinks, then writes a grammar-constrained answer": passed (7)
- "Without a thinking budget a thinking model answers straight away": passed (5)
- "A model without a thinking mode refuses a thinking budget": passed (1)

Consumer check:

```
$ cmake -S cmake/consumer-check -B build/consumer-check && cmake --build build/consumer-check --config Debug && ./build/consumer-check/Debug/consumer_check.exe
consumer_check: ok (rolled 15, total 18)
```

(`Morpheus::llamaserver` exists, and morpheus_llamaserver.lib is built in the vendored tree.)

## Byte checks

`check_bytes` on all 15 committed files: printed nothing.
`LC_ALL=C grep -c $'[\x80-\xFF]'` and the control-byte count `[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]`: 0 and 0 for every file.
The escapes are still literal backslash sequences: `grep -F '\x1E'` finds `'\x1E'` in Llm.cpp:13, the test literals in LlmTests.cpp:64-85 and LlamaServerBackendTests.cpp:155,157,200,203; `grep -F '\u001e'` finds LlamaServerBackendTests.cpp:116; the `\n` in the THINKING_* constants are intact (Llm.h:85-90).

## Files changed

- src/inference/Llm.h, src/inference/Llm.cpp
- src/llama/LlamaCppBackend.h, src/llama/LlamaCppBackend.cpp
- src/llamaserver/LlamaServerBackend.h, src/llamaserver/LlamaServerBackend.cpp, src/llamaserver/CMakeLists.txt (new)
- CMakeLists.txt, tests/CMakeLists.txt
- tests/inference/LlmTests.cpp
- tests/llamaserver/ServerTestSupport.h, tests/llamaserver/LlamaServerBackendTests.cpp (new)
- CLAUDE.md, README.md, cmake/consumer-check/CMakeLists.txt

## Deviations from the brief

1. tests/inference/LlmTests.cpp also gets `#include <catch2/matchers/catch_matchers_string.hpp>`: `CHECK_THROWS_WITH` is not defined by catch_test_macros.hpp alone (the RED build showed it). Other tests include the same header.
2. LlmTests.cpp and Llm.h had CRLF working-tree endings (core.autocrlf=true, LF in the index); I rewrote them with LF. The commit has no line-ending churn: the index stays LF.
3. CLAUDE.md, README.md and cmake/consumer-check/CMakeLists.txt are changed in this commit, at the dispatch's request (the plan puts this in Task 11). The wording is taken from Task 11 where it applied. Task 11's own "replace" texts for these files no longer match exactly: Task 4 and this task already changed those lines. Task 11 will need to adapt (the consumer-check `Morpheus::llamaserver` check and the `src/llamaserver` layout lines are already in place). The architecture spec is left to Task 11.
4. Commit trailer: `Co-Authored-By: Claude Opus 5.5`, per the instructions for an Opus implementer.

## Concerns

- On this machine the full `[llama]` run with MORPHEUS_TEST_THINKING_MODEL set to the 9B model runs out of Vulkan memory once the earlier tests have run in the same process. Each thinking test passes when run alone. This is an environment limit: the failure is a model load, before any TokenizePrompt call. I did not re-run it at the base commit. A smaller Qwen3.5 would probably avoid it.
- The LlamaCppBackend.h `afterEnd` comment, verbatim from the brief, is 144 columns: longer than the rest of the file (about 135 maximum). No formatter enforces a limit.
- Nothing is left running in the background: the dev build finished with exit 0.

## Fix round (review of ff92701)

Commit: 1c4cad2 "LlamaServerBackend: validate URLs before making a client, keep one connection per server, tell BOS from EOS".

### What changed

1. Malformed port crash.
   - The constructor now reads each URL itself: `http://`, then a host (ASCII letters, digits, `.`, `-`), then optionally `:` and a port of digits only, 1..65535, and nothing else. Anything else throws `InferenceError` naming the URL.
   - The client is made with `httplib::Client(host, port)`, so cpp-httplib's own scheme_host_port parsing (which leaves the client null) is never used. `is_valid()` is checked before any use and throws `InferenceError` naming the URL if false.
   - Refused URLs now tested: `http://127.0.0.1:80800`, `:65536`, `:0`, `:abc`, `:` (empty port), `:+80`, `http://:8080`, `http://user:pw@127.0.0.1:8080`, `http://127.0.0.1:8080?x=1`, `http://127.0.0.1 :8080`. A bad embedding URL (`http://127.0.0.1:0`) is also tested.
2. Persistent connections (owner decision).
   - Members `std::unique_ptr<httplib::Client> _pChatClient` and `_pEmbeddingClient` are created and validated in the constructor, set to keep-alive and reused by every request.
   - `httplib::Client` is forward-declared in the header, so only the .cpp includes <httplib.h>. Because of that, the destructor is defined out of line and copying is deleted.
   - `Ask` takes the client and the URL (the URL only for error messages).
   - The class comment says the streaming /completion may use a client of its own so a cancel can stop it (for Task 6), and why connections are kept: about 2 s per connect on Windows when "localhost" tries ::1 first.
   - The backend stays single-threaded; there is no lock.
3. Minors.
   - BOS detection: `/tokenize` of "" with add_special returning more than one token throws `InferenceError` naming the chat URL ("adds 2 special tokens to a text ..."). With exactly one, the backend tokenizes `BOS_PROBE_TEXT` ("a") with add_special. The token is a BOS only if it comes first; an EOS appended after the text is ignored, and the backend never asks for it. That costs one extra request at startup, and only for models that add one special token.
   - `READ_TIMEOUT_SECONDS` is now 60, for /props, /tokenize, /apply-template and /embedding. The long wait for /completion is left to Task 6's own client, as the comment says. No unused constant was added.
   - New tests: an HTTP 500 from /apply-template and a non-JSON body from /tokenize during `CountPromptTokens`, each naming URL+path and quoting the body or status.

Fake additions in `tests/fakes/FakeLlamaServer.h`, each covered by a new `[fake-llama-server]` test case:
- `Request::remotePort` (the client's port, which identifies its connection);
- `SetRawAnswer(path, body)` (HTTP 200 with a body that is not JSON);
- `explicit FakeLlamaServer(int port = 0)` (listen on a given port, as a restarted server would);
- `Port()`.

### Restart behaviour: what actually happens (tested)

With the chat fake destroyed, the next request throws `InferenceError` "cannot reach <url>/tokenize: ...". cpp-httplib sees the kept-alive socket is dead, and the reconnect fails. Once a new fake listens on the same port, the next request succeeds: cpp-httplib reconnects by itself, with no backend code involved. The test "LlamaServerBackend keeps one connection to each server and reconnects after a restart" also checks:
- the first backend's 9 chat requests used 1 connection, and its 3 embedding requests used 1;
- a second backend instance on the same fakes works and has a connection of its own (2 per server in total);
- the first backend keeps working after the second one is destroyed.

### TDD evidence

RED 1, build (tests written, fake not yet extended):

```
FakeLlamaServerTests.cpp(203,28): error C2039: 'Port': is not a member of 'morpheus::test::FakeLlamaServer'
FakeLlamaServerTests.cpp(217,5): error C2039: 'remotePort': is not a member of 'morpheus::test::FakeLlamaServer::Request'
FakeLlamaServerTests.cpp(222,11): error C2039: 'SetRawAnswer': is not a member of 'morpheus::test::FakeLlamaServer'
LlamaServerBackendTests.cpp(343,28): error C2039: 'Port': is not a member of ...
LlamaServerBackendTests.cpp(409,14): error C2039: 'SetRawAnswer': is not a member of ...
```

RED 2, run (fake extended, backend unchanged):

```
"a URL that is not http://host:port": FAILED ... due to a fatal error condition: http://127.0.0.1:80800  SIGSEGV - Segmentation violation signal
"keeps one connection ... restart": connections(chat->Requests()) == 1 for: 9 == 1; connections(embed.Requests()) == 1 for: 3 == 1;
    == 2 for: 13 == 2; == 2 for: 4 == 2
"tells a BOS ... from an EOS": CountPromptTokens == ... for: 26 == 25; add_special == false for: true == false;
    expected exception, got none (x2, the "2 special tokens" section)
"names the URL when counting a prompt's tokens fails": passed already (Ask had this behaviour; the test adds coverage)
[fake-llama-server]: All tests passed (67 assertions in 6 test cases)
```

GREEN:

```
$ cmake --build --preset tests                                   -> no warnings
$ ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"
All tests passed (186 assertions in 19 test cases)
$ ctest --preset tests
100% tests passed out of 395                                      (391 + 1 fake case + 3 backend cases)
$ cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe
exit 0, no warnings or errors outside _deps
$ ./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llamaserver],[fake-llama-server]"
All tests passed (186 assertions in 19 test cases)
$ consumer check (rebuilt): consumer_check: ok (rolled 15, total 18)
```

The `[llama]` model tests were not re-run, as instructed: src/llama and src/inference are unchanged in this round.

### Byte checks

`check_bytes` on the 5 changed files printed nothing. The non-ASCII and control-byte counts are 0 and 0 for each file. In LlamaServerBackendTests.cpp, `grep -cF '\x1E'` = 4 and `grep -cF '\u001e'` = 1, as before: the escapes are still literal.

### Files changed

src/llamaserver/LlamaServerBackend.h, src/llamaserver/LlamaServerBackend.cpp, tests/fakes/FakeLlamaServer.h, tests/llamaserver/FakeLlamaServerTests.cpp, tests/llamaserver/LlamaServerBackendTests.cpp.

### Notes and concerns

- Hosts are limited to ASCII letters, digits, `.` and `-`, so an IPv6 literal (`http://[::1]:8080`) is refused. Settings and the docs use 127.0.0.1. Say if IPv6 literals are wanted.
- cpp-httplib does not retry a request that fails on a kept-alive socket the server closed during the request itself (a race with the server's keep-alive timeout). Such a request throws `InferenceError` naming the URL, and the next one reconnects. Before sending, cpp-httplib checks whether the socket is alive, so this needs the close to land mid-request.
- The restart test binds the port the stopped fake used. That worked on this machine (Windows) and on every run. If a platform ever refuses the rebind, the fake throws "cannot bind port N of 127.0.0.1".
- Nothing is left running in the background: the dev build ran in the foreground and finished.
