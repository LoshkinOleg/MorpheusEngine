# Morpheus Engine

A local-first, LLM-driven text RPG engine in C++20. The engine owns the game (structured world state, dice, memory); a local model running in-process through llama.cpp proposes outcomes and narrates them.

The earlier C# implementation lives on the `master` branch. This branch (`cpp`) is the rewrite.

## Design

- [Architecture](docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md)
- [Narrative visualization](docs/superpowers/specs/2026-10-27-narrative-visualization-design.md)
- [Narration text-to-speech](docs/superpowers/specs/2026-10-27-narration-tts-design.md)
- [Implementation plans](docs/superpowers/plans)

## Build (Windows)

Prerequisites: Visual Studio 2022 with the C++ workload, CMake 3.25+, Git. CMake fetches every dependency at a pinned commit.

| Preset | Builds |
|---|---|
| `tests` | Engine libraries and tests only (no llama.cpp, SDL3 or ImGui) |
| `asan` | Same as `tests`, instrumented with AddressSanitizer |
| `dev` | Everything, including the `morpheus` app |

```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

## Layout

- `src/core`: world loading, schema-typed state, state changes, dice
- `src/store`: one SQLite database per run
- `src/main.cpp`: prototype app (replaced in milestone 4)
- `tests/`: Catch2 tests and test data
- `game_projects/<id>/`: worlds as data
