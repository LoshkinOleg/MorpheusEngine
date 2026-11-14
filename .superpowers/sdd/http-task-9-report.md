# Task 9 report: the callers build the backend settings.json names

Status: DONE

Commit: d5fdf96 The app, morpheus_play and morpheus_probe build the backend settings.json names

## What was implemented

- `app::EngineHost::Start` keeps the loaded backend only when the backend kind, the model paths, the server URLs
  (`chatUrl`, `embeddingUrl`), the slot (`serverSlot`), the embedding prefixes, the context size and the GPU layers
  are all unchanged. The `Start` comment says so.
- New free function `app::BackendFactoryByKind(llamaCpp, llamaServer)` in `src/app/model/EngineHost.h/.cpp`: it
  requires both factories (`MORPHEUS_REQUIRE`, ContractError) and returns a factory that switches on
  `settings.backend`.
- `src/app/main.cpp`: new `MakeServerBackend` (builds `llamaserver::LlamaServerConfig` from the settings);
  `AppController` gets `app::BackendFactoryByKind(MakeLlamaBackend, MakeServerBackend)`. `MakeLlamaBackend`'s comment
  updated. `morpheus` links `morpheus_llamaserver`.
- `tools/play/main.cpp` and `tools/probe/main.cpp` build `LlamaCppBackend` or `LlamaServerBackend` by
  `settings.backend` (through `std::unique_ptr<inference::LlmBackend>`); the probe prints
  `backend: llama-server at <url> (slot N)` for the server, and its header comment was updated. Both link
  `morpheus_llamaserver`.
- All code is verbatim from the brief. The brief's code matched the current interfaces (`LlamaServerConfig`'s
  fields, `core::BackendKind`, `Settings::backend/chatUrl/embeddingUrl/serverSlot`, `SettingsEdit`'s backend fields),
  so no interface deviations were needed. This makes the Settings window's promise (the backend is rebuilt when the
  backend, a URL or the slot changes) true.

## TDD evidence

RED 1: `cmake --build --preset tests` with both tests appended:

```
D:\MorpheusEngine\tests\app\AppControllerTests.cpp(295,60): error C2039: 'BackendFactoryByKind': is not a member of 'morpheus::app'
D:\MorpheusEngine\tests\app\AppControllerTests.cpp(295,60): error C3861: 'BackendFactoryByKind': identifier not found
```

RED 2: AppController test temporarily removed, `./build/tests/bin/Debug/morpheus_tests.exe "[engine_host]"`:

```
EngineHostTests.cpp(142): FAILED:  CHECK( made == 2 )  with expansion:  1 == 2
EngineHostTests.cpp(147): FAILED:  CHECK( made == 3 )  with expansion:  1 == 3
EngineHostTests.cpp(152): FAILED:  CHECK( made == 4 )  with expansion:  1 == 4
EngineHostTests.cpp(155): FAILED:  CHECK( made == 5 )  with expansion:  1 == 5
test cases:  5 |  4 passed | 1 failed
assertions: 40 | 36 passed | 4 failed
```

(The AppController test was restored before implementing.)

GREEN:
- `./build/tests/bin/Debug/morpheus_tests.exe "[engine_host],[app_controller]"`: `All tests passed (223 assertions in 16 test cases)`.
- `ctest --preset tests`: `100% tests passed out of 423` (421 at base + 2 new).
- `cmake --build --preset asan` (exit 0, no warnings) then `ctest --preset asan`: `100% tests passed out of 423` (221.8 s).
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`: exit 0, no warnings or errors.
- `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120` from the repo root: exit 0.
- `morpheus_probe settings.json game_projects/sandcrawler "I look around."` (this machine's settings, backend
  `llama-cpp`, models present): exit 0; run 1 `prompt 719 tokens: 0 reused, 719 evaluated in 221 ms; 124 generated`,
  run 2 `prompt 719 tokens: 718 reused, 1 evaluated in 6 ms; 125 generated`.
- Server error path (brief Step 6, `build/server-smoke/settings.json` with `"backend": "llama-server"` and both URLs
  `http://127.0.0.1:1`): `error: cannot reach http://127.0.0.1:1/props: Could not establish connection`, `exit 1`.

## Byte checks (all 10 committed files)

- `check_bytes ...`: printed nothing.
- `LC_ALL=C grep -n $'[\x80-\xFF]' <file>`: printed nothing for every file.
- `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' <file>`: 0 for every file.
- The one escape in this task, the `\n` in the probe's `std::format("backend: llama-server at {} (slot {})\n", ...)`,
  was decoded by the tool on the first write (it became a real line break inside the string literal). Rewritten with
  a constructed backslash; `grep` now shows the literal `\n` on line 53 of `tools/probe/main.cpp`.

## Files changed

- D:/MorpheusEngine/src/app/model/EngineHost.h
- D:/MorpheusEngine/src/app/model/EngineHost.cpp
- D:/MorpheusEngine/src/app/main.cpp
- D:/MorpheusEngine/src/app/CMakeLists.txt
- D:/MorpheusEngine/tools/play/main.cpp
- D:/MorpheusEngine/tools/play/CMakeLists.txt
- D:/MorpheusEngine/tools/probe/main.cpp
- D:/MorpheusEngine/tools/probe/CMakeLists.txt
- D:/MorpheusEngine/tests/app/EngineHostTests.cpp
- D:/MorpheusEngine/tests/app/AppControllerTests.cpp

## Deviations from the brief

- None in code. Test counts differ from the brief's stale figure (409): 423 = 421 + 2, as the controller said.
- `morpheus.exe` was run with `--frames 120` (controller) rather than the brief's `--frames 3`.
- The commit trailer names Claude Opus 5.5 (the implementing model), per the global constraints.

## Concerns

- None blocking. `morpheus_play` was built but not run interactively (the brief does not ask for it); its
  llama-server path is the same construction as the probe's, which was exercised against an unreachable server.
- `build/server-smoke/settings.json` was left in the gitignored build tree.
