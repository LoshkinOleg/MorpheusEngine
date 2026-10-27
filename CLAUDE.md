# MorpheusEngine

Local-first, LLM-driven text RPG engine in C++20. The repository owner acts as product designer; Claude implements.

## Design documents
- Architecture: docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
- Narrative visualization: docs/superpowers/specs/2026-10-27-narrative-visualization-design.md
- Narration TTS: docs/superpowers/specs/2026-10-27-narration-tts-design.md
- Implementation plans: docs/superpowers/plans/

## Build and test (Windows, Visual Studio 2022, CMake >= 3.25, Git Bash)
- Fast loop (engine libraries + tests, no llama.cpp/SDL/ImGui): `cmake --preset tests`, `cmake --build --preset tests`, `ctest --preset tests`
- AddressSanitizer: the same three commands with preset `asan`
- `tests` and `asan` build without llama.cpp. `dev` builds everything, including llama.cpp with Vulkan (needs the Vulkan SDK), the app and `morpheus_probe`.
- Model-backed tests are tagged `[llama]` and skip unless `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL` point at the two GGUF files in `models/`.
- One tag or test case: `./build/tests/bin/Debug/morpheus_tests.exe "[csv]"`

## Layout
- src/core: world loading, settings, schema-typed state, state changes, dice, turn record types. No LLM, no UI; its only I/O is reading world folders.
- src/store: one SQLite database per run.
- src/inference: the LlmBackend interface and its types. No llama.cpp.
- src/context: layered prompt assembly with exact token budgets.
- src/llama: the llama.cpp backend (`dev` preset only). Only this module and tools/probe include <llama.h>.
- tools/probe: `morpheus_probe`, a manual check of the models, the Vulkan backend and KV-cache reuse (`dev` preset only).
- models/ and settings.json: both gitignored, per machine. settings.example.json is the template.
- src/main.cpp: prototype app, replaced in milestone 4.
- tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds.
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
