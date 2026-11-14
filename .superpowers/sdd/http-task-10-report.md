# Task 10 report: the [server] suite

Status: DONE

Commit: ec71652 Add the [server] tests: a real llama-server, token agreement, and R2's three-slot reuse

## What was implemented

- `tests/TestEnvironment.h`: `test::EnvironmentVariable`, moved unchanged from `tests/llama/LlamaTestModels.h`.
- `tests/llama/LlamaTestModels.h`: the function and `#include <cstdlib>` removed; `#include "TestEnvironment.h"` added before `llama/LlamaCppBackend.h`.
- `tests/llamaserver/LiveServer.h`: `test::LIVE_CONTEXT_SIZE = 16384` and `test::LiveServerConfig(int slot)`, which SKIPs unless `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL` are set.
- `tests/llamaserver/LlamaServerLiveTests.cpp` (`[server]`, every preset with the llama-server backend): three tests, all verbatim from the brief:
  - a resolve call under the sandcrawler resolve grammar (checked with `grammar::Check`), then a streamed narrate call on the same slot;
  - a thinking call, which skips when the model has no thinking mode;
  - R2: three sessions on three threads and slots, 10 exchanges each. Each exchange makes an action call under a grammar and a speech call. The test records the reuse fraction per call, checks every call after the first reuses more than 0, and WARNs the figures.
- `tests/llama/LlamaServerAgreementTests.cpp` (`[server][llama]`, dev only): `CountTokens` and `CountPromptTokens` agree between the server and `test::SharedBackend()`. It skips unless the server's `/props` `model_path` file name equals `MORPHEUS_TEST_CHAT_MODEL`'s.

The code is the brief's, verbatim. I checked it against the current headers and every API it uses matched, so I needed no changes:
- `LlamaServerConfig` fields and the constructor;
- `grammar::Check`;
- `inference::Completion` and `GenerationSpec` fields;
- `context::RenderWorldReference` and `RenderStateView`;
- `turn::BuildResolveGrammar(world, state, MemoryMode)`;
- `core::LoadWorld`;
- `test::RepoDir`;
- the "no thinking mode" text of `LlamaServerBackend`'s InferenceError.

URLs appear only in `LiveServer.h`'s comment and use 127.0.0.1. The brief doesn't touch `core::Settings` (`BackendKind`, `chatUrl`, `embeddingUrl`, `serverSlot`).

## Evidence

### RED substitute: unreachable server (tests preset)
`MORPHEUS_TEST_SERVER_URL=http://127.0.0.1:1 MORPHEUS_TEST_EMBED_SERVER_URL=http://127.0.0.1:1 ./build/tests/bin/Debug/morpheus_tests.exe "[server]"`
```
llama-server: three sessions on three slots reuse their own cached prompts
LlamaServerLiveTests.cpp(203): FAILED:
  CHECK( log.error == "" )
with expansion:
  "cannot reach http://127.0.0.1:1/props: Could not establish connection" == ""
with message:
  session 0
LlamaServerLiveTests.cpp(204): FAILED:
  REQUIRE( log.reused.size() == static_cast<std::size_t>(2 * EXCHANGES) )
with expansion:
  0 == 20
llama-server: a resolve call completes under its grammar and a narrate call streams
LlamaServerLiveTests.cpp(70): FAILED:
due to unexpected exception with message:
  cannot reach http://127.0.0.1:1/props: Could not establish connection
llama-server: a thinking model thinks before it answers under the grammar
LlamaServerLiveTests.cpp(114): FAILED:
due to unexpected exception with message:
  cannot reach http://127.0.0.1:1/props: Could not establish connection
test cases: 3 | 3 failed
assertions: 4 | 4 failed
exit 42
```
The dev build gives the same three failures. With only the URLs set, the agreement test skips at `SharedBackend`. With the model variables set as well (Qwen3-4B, nomic), it fails loudly on the unreachable server:
```
LlamaServerAgreementTests.cpp(31): FAILED:
  REQUIRE( props )
with expansion:
  0
test cases: 1 | 1 failed
```

### GREEN (skips cleanly, nothing set)
- `cmake --build --preset tests`: no warnings or errors.
- `./build/tests/bin/Debug/morpheus_tests.exe "[server]"`: `test cases: 3 | 3 skipped`, each with "set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL to run the [server] tests".
- `ctest --preset tests`: `100% tests passed out of 426` (423 + 3), with tests 416-418 (the three `llama-server: ...` tests) "Not Run (Skipped)".
- `cmake --build --preset asan`: no compiler warnings. The only warning is the linker's usual `LNK4300: ignoring '/INCREMENTAL' because input module contains ASAN metadata`, which every asan build gives.
- `ctest --preset asan`: `100% tests passed out of 426`, the same three skipped.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`: no warnings. The only warning is llama.cpp's own "ccache not found" from its configure.
- `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[server]"`: `test cases: 4 | 4 skipped`. The three live tests skip at `LiveServerConfig`; the agreement test skips at `SharedBackend` ("set MORPHEUS_TEST_CHAT_MODEL and MORPHEUS_TEST_EMBED_MODEL ...").

### Byte checks
- `check_bytes` on the five files printed nothing.
- `LC_ALL=C grep -c $'[\x80-\xFF]'`: 0 for all five.
- The control-byte grep (`[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]`): 0 for all five.
- `grep -n 'xC3' tests/llama/LlamaServerAgreementTests.cpp` shows line 42 with the literal `"caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC"`.
- `ACTION_GRAMMAR` keeps its literal `\\\"` escapes.

## Files changed
- D:/MorpheusEngine/tests/TestEnvironment.h (new)
- D:/MorpheusEngine/tests/llama/LlamaTestModels.h
- D:/MorpheusEngine/tests/llamaserver/LiveServer.h (new)
- D:/MorpheusEngine/tests/llamaserver/LlamaServerLiveTests.cpp (new)
- D:/MorpheusEngine/tests/llama/LlamaServerAgreementTests.cpp (new)

## Deviations from the brief
- **Commit trailer:** `Co-Authored-By: Claude Opus 5.5`, as the global constraints require for an Opus implementer.
- **Test counts:** 426 instead of the brief's stale 412 (the base passes 423, plus 3).
- **No code changes.**

## Concerns / notes
- **Dev build needs two passes:** the first `cmake --build build/dev ... --target morpheus_tests` after adding files only regenerates the project (the CONFIGURE_DEPENDS re-glob). MSBuild then builds the old file list, and the binary has no new tests ("No test cases matched '[server]'"). A second build compiles them. This is expected CMake/MSBuild behaviour, not a defect, but worth knowing when the owner adds test files and builds dev.
- **Not verified here:** whether the tests pass against a real server, since none runs on this machine. The owner runs them and records the R2 reuse figures (README, Task 11).
- **Background work:** the `ctest --preset asan` run was moved to the background by the tool after 180 s. I waited for it to finish (exit 0) before going on. Nothing is left running.
