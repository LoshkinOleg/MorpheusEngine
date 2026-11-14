# MorpheusEngine

Local-first, LLM-driven text RPG engine in C++20. The repository owner acts as product designer; Claude implements.

## Design documents
- Architecture: docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
- Narrative visualization: docs/superpowers/specs/2026-10-27-narrative-visualization-design.md
- Narration TTS: docs/superpowers/specs/2026-10-27-narration-tts-design.md
- Implementation plans: docs/superpowers/plans/

## Build and test (CMake >= 3.25; Windows: Visual Studio 2022, Git Bash; Linux: clang, Ninja)
- Fast loop (engine libraries + tests: no llama.cpp or SDL3, but ImGui's core is built and the tests draw the app's windows headless): `cmake --preset tests`, `cmake --build --preset tests`, `ctest --preset tests`
- AddressSanitizer: the same three commands with preset `asan`
- `tests` and `asan` build without llama.cpp. `dev` builds everything, including llama.cpp with Vulkan (needs the Vulkan SDK), the app (`morpheus`, the startup project in Visual Studio, run from the repo root), `morpheus_probe` and `morpheus_play`.
- `dev-asan` is `dev` with llama.cpp on the CPU (no Vulkan SDK needed) and every target under AddressSanitizer, RelWithDebInfo: the tests, `morpheus_probe` and `morpheus_play`, not the app. Slow, for memory checks of the backend. Binaries land in `build/dev-asan/bin/RelWithDebInfo/`.
- `MORPHEUS_GPU_BACKEND` picks llama.cpp's GPU backend: `Vulkan` (the default; `dev`), `CUDA` or `CPU` (`dev-asan`). Any other value fails the configure.
- `MORPHEUS_BUILD_SERVER` (default on when top level) builds `morpheus_server` and its library `morpheus_server_lib` in every preset, without llama.cpp; it needs `MORPHEUS_BUILD_LLAMA_SERVER`.
- Linux (Ubuntu 24.04): `linux-tests` is the fast loop (`cmake --preset linux-tests`, `cmake --build --preset linux-tests`, `ctest --preset linux-tests`; binaries in `build/linux-tests/bin/`); `linux-release` builds everything with llama.cpp on CUDA (needs the CUDA toolkit). Each OS lists only its own presets.
- Included in another CMake project (not top level), Morpheus builds only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn`, `Morpheus::llamaserver`) and cpp-httplib as `morpheus_httplib` (fetched when `MORPHEUS_BUILD_LLAMA_SERVER` is on: the default, also when vendored; header-only, no OpenSSL), with warnings that are not errors (`MORPHEUS_WARNINGS_AS_ERRORS`) and the host's output directories. `cmake/consumer-check/` checks this (README, "Using Morpheus from another project").
- Model-backed tests are tagged `[llama]` and skip unless `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL` point at the two GGUF files in `models/`.
- Real-server tests are tagged `[server]` and skip unless `MORPHEUS_TEST_SERVER_URL` and `MORPHEUS_TEST_EMBED_SERVER_URL` point at running llama-servers (README, "The [server] tests"); `[server]` also covers `morpheus_server` over HTTP.
- The server's own tests that need no real server are tagged `[hosted-server]`.
- One tag or test case: `./build/tests/bin/Debug/morpheus_tests.exe "[csv]"`

## Layout
- src/core: world loading, settings, schema-typed state, state changes, dice, turn record types. No LLM, no UI; its only I/O is reading world folders and settings.json.
- src/store: one SQLite database per run.
- src/inference: the Tokenizer, ChatModel and Embedder interfaces, LlmBackend (their union) and their types. No llama.cpp, no HTTP.
- src/context: layered prompt assembly with exact token budgets.
- src/grammar: bounded-JSON grammars: the GBNF builder, `FromSchema`, `Validate` and `grammar::Check` (does a text fit a grammar, and where it stops fitting). No llama.cpp.
- src/turn: turn orchestration: grammar, resolve and apply, retrieval, folding and the shared memory upkeep (`MemoryUpkeep`), session, worker, replay; hosted runs, a world the host owns and sends as data (`HostedWorld`, `HostedSession`).
- src/llama: the llama.cpp backend (`dev` and `linux-release` presets only). Only this module, tools/probe, tools/play and src/app/main.cpp link llama.cpp; only src/llama and the tools include <llama.h>.
- src/llamaserver: the llama-server backend over HTTP (cpp-httplib, header-only, no OpenSSL), built in every preset (`MORPHEUS_BUILD_LLAMA_SERVER`, default on). Only it, src/server/HttpServer.cpp and the tests include <httplib.h>; tests/fakes/FakeLlamaServer.h is the fast suite's fake server.
- src/server: `morpheus_server_lib`, the server for hosted runs: `SlotPool` and `SlotBackend` (a llama-server slot per conversation, slot 0 for folding), `RunWorker` (one thread per run owning its `HostedSession` and backend, folding when idle), `RunRegistry` (run ids and files), `Api` (every route and status, without sockets), `TurnStream`, `SentenceSplitter`, `HttpServer` (cpp-httplib, Server-Sent Events). Never links llama.cpp.
- tools/server: `morpheus_server` (`--port`, `--runs`, `--settings`), every top-level preset.
- tools/probe: `morpheus_probe`, a manual check of the models, the GPU backend and KV-cache reuse (`dev` and `linux-release` presets only).
- tools/play: `morpheus_play`, the terminal player; `--record` writes a replay fixture for tests/data/replays (`dev` and `linux-release` presets only).
- models/ and settings.json: both gitignored, per machine. settings.example.json is the template.
- src/app: the app. model/ (morpheus_app_model): its state and actions, no SDL, ImGui or llama.cpp. ui/ (morpheus_app_ui): the ImGui windows, needing only ImGui's core, so tests draw them headless (tests/app/HeadlessImGui.h). main.cpp (`morpheus`, `dev` and `linux-release` presets only): the SDL3 window and the backend settings.json names (llama.cpp in-process or llama-server). ImGui is upstream's docking branch; IM_ASSERT throws (src/app/ui/ImGuiConfig.h).
- tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds, tests/hosted/fixtures: hosted-run fixtures written from the host's data contracts.
- cmake/consumer-check: a host project that includes Morpheus through FetchContent and links `Morpheus::turn`; it fails to configure if the vendored build defines more than the engine libraries and `morpheus_httplib`.
- game_projects/<id>/: worlds as data (world.json, rules.md, lore.csv, state_schema.json, initial_state.json).

## Coding rules
- Fail fast and loud. Engine bugs throw core::ContractError via MORPHEUS_REQUIRE. Malformed world files or saved data throw core::DataError / store::StoreError naming the file and field. Mistakes in LLM output are not exceptions: they are rejected with a reason, logged, and the turn continues. Never add fallbacks that hide a broken contract.
- Method bodies carry enough comments to read top to bottom without guessing intent.
- Extract a private helper only when it has more than one caller. Single-use logic stays inline; local lambdas are fine for organisation. Public single-use methods are fine.
- Class layout order: nested types, public data, private data, public methods, private methods, helpers.
- Constants (constexpr, static const) are ALL_CAPS and grouped together.
- Naming: types and methods PascalCase; locals and parameters camelCase; members _camelCase; raw pointers pName / _pName.
- Initialize members at declaration, even to their default value.
- RAII for every resource. No naked new/delete. Raw pointers never own.
- Prefer methods over getters when a value is computed rather than stored.
- When changing code, don't keep legacy paths for old behavior; update callers to the new contract.
- ASCII only in source comments and docs. docs/ holds documentation only.
- Each module is a static library and dependencies only flow downward (see the architecture spec).
- New behavior lands with Catch2 tests, written first.
