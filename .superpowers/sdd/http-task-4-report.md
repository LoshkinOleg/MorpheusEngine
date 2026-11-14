# Task 4 report: cpp-httplib and the fake llama-server

Status: DONE. Commit 8dd812f "Fetch cpp-httplib behind MORPHEUS_BUILD_LLAMA_SERVER; add a fake llama-server for the tests".

## Implemented
- Root CMakeLists.txt: option MORPHEUS_BUILD_LLAMA_SERVER (ON), the configure error for LLAMA without LLAMA_SERVER, FetchContent of cpp-httplib at cf3693cb (SOURCE_SUBDIR no-cmake-build, download only), INTERFACE target morpheus_httplib (SYSTEM include, Threads, ws2_32, /bigobj). Exactly as the brief.
- tests/CMakeLists.txt: llamaserver sources filter and the morpheus_httplib link.
- tests/fakes/FakeLlamaServer.h and tests/llamaserver/FakeLlamaServerTests.cpp: verbatim from the brief.

## TDD evidence
- RED: with the new option line and the tests/CMakeLists.txt changes in, but no fake header and no morpheus_httplib target: `cmake --build --preset tests` ->
  `FakeLlamaServerTests.cpp(11,1): error C1083: Cannot open include file: 'fakes/FakeLlamaServer.h': No such file or directory`
  (Note: the option line had to exist before RED, else the llamaserver sources are filtered out and nothing fails.)
- GREEN: `cmake --build --preset tests` clean (no warnings, as /WX would fail), `morpheus_tests.exe "[fake-llama-server]"` -> `All tests passed (46 assertions in 3 test cases)`.
- `ctest --preset tests`: 100% tests passed out of 377.

## dev build and no clash
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`: re-ran CMake, exit 0, no compiler warnings (only llama.cpp's own "ccache not found" configure notice).
- Verified no clash: build/dev/_deps holds morpheus_cpp_httplib-src next to llama.cpp's vendored build/dev/_deps/llama-src/vendor/cpp-httplib (whose target is its own static lib); configure and build succeeded, and `build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[fake-llama-server]"` passes (46 assertions, 3 cases) in the dev tree.
- Consumer check: `cmake -S cmake/consumer-check -B <tmp>` configures (also fetched/linked cpp-httplib through the vendored build). morpheus_httplib is not in its unwanted-target list and I left that file unchanged (not built, configure only; temp dir removed).

## Byte checks
- check_bytes on CMakeLists.txt, tests/CMakeLists.txt, FakeLlamaServer.h, FakeLlamaServerTests.cpp: no output.
- `grep -n $'[\x80-\xFF]'` on the two new files: no output; control-byte count: 0 and 0.
- The `\n` escapes in FakeLlamaServer.h are literal backslash-n (grep shows lines 81, 82, 85, 176, 180, 189, 218).

## Deviations / concerns
- None to the brief's code. Files are written LF; git warns that it will convert to CRLF (repo autocrlf), as for other files.
- The brief's expected ctest count (377) matched.

## Fix round
Commit: see `git log` (follows 8dd812f).
1. Callbacks (tokenizer, renderer, embedding maker) are copied under the lock (`Copy`) and run outside it; /completion copies the tokenizer too. New test: a tokenizer that calls `Requests()` while two threads hit /tokenize and wait for each other inside it. RED against the old header: `seen >= 1` failed (0) (the old code ran the callback under the lock), then green.
2. `_clientClosedStream` is set only when the client closed. No test: a later held stream ending by timeout (10 s) is too slow to observe; change is a 3-line guard.
3. `Record` stores the body without throwing (a non-JSON body is recorded as its text); new `ParseBody` answers 400 for a malformed body in the four POST handlers. New test (RED against the old header: status 500, request not recorded).
4. ALL_CAPS constants: DEFAULT_CONTEXT_SIZE, DEFAULT_SLOTS, HOLD_LIMIT_SECONDS, PING_INTERVAL_MS (the chatml default and token-size literals left inline).
5. CLAUDE.md (2 lines), README.md (2 lines) and cmake/consumer-check/CMakeLists.txt comments now say morpheus_httplib is fetched with MORPHEUS_BUILD_LLAMA_SERVER (default ON, also vendored); the check now requires `morpheus_httplib` to exist. `morpheus_llamaserver` is NOT mentioned or listed yet: Task 5 must add it to those places.
Verification: `[fake-llama-server]` 5 cases / 53 assertions pass; `ctest --preset tests` 379/379; consumer check configured and built from scratch (only a MSB8029 notice because its build dir was under %TEMP%); byte checks on all changed files print nothing. No build left running.
