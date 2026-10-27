# Morpheus Engine

A local-first, LLM-driven text RPG engine in C++20. The engine owns the game (structured world state, dice, memory); a local model running in-process through llama.cpp proposes outcomes and narrates them.

The earlier C# implementation lives on the `master` branch. This branch (`cpp`) is the rewrite.

## Design

- [Architecture](docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md)
- [Narrative visualization](docs/superpowers/specs/2026-10-27-narrative-visualization-design.md)
- [Narration text-to-speech](docs/superpowers/specs/2026-10-27-narration-tts-design.md)
- [Implementation plans](docs/superpowers/plans)

## Build (Windows)

Prerequisites: Visual Studio 2022 with the C++ workload, CMake 3.25+, Git, and the Vulkan SDK (only for the `dev` preset, which builds llama.cpp with Vulkan). CMake fetches every dependency at a pinned commit.

| Preset | Builds |
|---|---|
| `tests` | Engine libraries and tests only (no llama.cpp, SDL3 or ImGui) |
| `asan` | Same as `tests`, instrumented with AddressSanitizer |
| `dev` | Everything: llama.cpp with Vulkan, the `morpheus` app and the `morpheus_probe` tool |

```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

## Models and settings

The `dev` preset runs models in-process through llama.cpp. Download two GGUF files into `models/` (gitignored):

- `Qwen3-4B-Instruct-2507-Q4_K_M.gguf` (chat, about 2.5 GB) from Hugging Face `unsloth/Qwen3-4B-Instruct-2507-GGUF`
- `nomic-embed-text-v1.5.Q8_0.gguf` (embeddings, about 140 MB) from Hugging Face `nomic-ai/nomic-embed-text-v1.5-GGUF`

Then copy `settings.example.json` (the template) to `settings.json` at the repository root and adjust it for your machine. `settings.json` is gitignored. Relative model paths resolve against the settings file's folder.

To check that the models, the Vulkan backend and KV-cache prefix reuse work, run the probe. It prints the prompt size and streams two completions; the second should report almost the whole prompt as reused:

```
./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
```

llama.cpp prints a few warnings while loading Qwen3 (control-token and `n_ctx_seq` notices); they are expected. The two runs' text can differ slightly even with the same seed.

Model-backed tests are tagged `[llama]` and skip unless both model variables are set (Git Bash, from the repo root):

```
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/Debug/morpheus_tests.exe "[llama]"
```

## Layout

- `src/core`: world loading, settings, schema-typed state, state changes, dice, turn record types
- `src/store`: one SQLite database per run
- `src/inference`: the `LlmBackend` interface (no llama.cpp)
- `src/context`: layered prompt assembly with exact token budgets
- `src/llama`: the llama.cpp backend (`dev` preset only)
- `tools/probe`: `morpheus_probe`, a manual model check (`dev` preset only)
- `src/main.cpp`: prototype app (replaced in milestone 4)
- `tests/`: Catch2 tests and test data
- `game_projects/<id>/`: worlds as data
