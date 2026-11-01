# Morpheus Engine

A local-first, LLM-driven text RPG engine in C++20. The engine owns the game (structured world state, dice, memory); a local model running in-process through llama.cpp proposes outcomes and narrates them.

The earlier C# implementation lives on the `master` branch. This branch (`cpp`) is the rewrite.

## Design

- [Architecture](docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md)
- [Narrative visualization](docs/superpowers/specs/2026-10-27-narrative-visualization-design.md)
- [Narration text-to-speech](docs/superpowers/specs/2026-10-27-narration-tts-design.md)
- [Implementation plans](docs/superpowers/plans)

## Build (Windows)

Prerequisites: Visual Studio 2022 with the C++ workload, CMake 3.25+, Git, and the Vulkan SDK (only for the `dev` preset, which builds llama.cpp with Vulkan; `dev-asan` runs llama.cpp on the CPU and needs no Vulkan SDK). CMake fetches every dependency at a pinned commit.

| Preset | Builds |
|---|---|
| `tests` | Engine libraries and tests only (no llama.cpp or SDL3) |
| `asan` | Same as `tests`, instrumented with AddressSanitizer |
| `dev` | Everything: llama.cpp with Vulkan, the `morpheus` app and the `morpheus_probe` and `morpheus_play` tools |
| `dev-asan` | Like `dev`, but llama.cpp on the CPU (no Vulkan) and every target under AddressSanitizer, RelWithDebInfo: the tests, `morpheus_probe` and `morpheus_play`, not the app. Slow, for memory checks of the backend |

```
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

## Models and settings

The `dev` preset runs models in-process through llama.cpp. Download two GGUF files into `models/` (gitignored):

- `Qwen3.8-27B-UD-Q4_K_M.gguf` (chat, a thinking model, about 16.5 GB) from Hugging Face `unsloth/Qwen3.8-27B-GGUF`
- `nomic-embed-text-v1.5.Q8_0.gguf` (embeddings, about 140 MB) from Hugging Face `nomic-ai/nomic-embed-text-v1.5-GGUF`

The model-backed tests also use `Qwen3-4B-Instruct-2507-Q4_K_M.gguf` (about 2.5 GB, `unsloth/Qwen3-4B-Instruct-2507-GGUF`), a model without a thinking mode, which also plays much faster on small GPUs.

Then copy `settings.example.json` (the template) to `settings.json` at the repository root and adjust it for your machine. `settings.json` is gitignored. Relative model paths resolve against the settings file's folder.

- `budget.retrieval_cap` caps the retrieved memory (lore, past turns, notes) placed in the prompt, in tokens, including what `assisted`-mode lookups add. It is part of `budget.tail_allowance`.
- The optional top-level `"memory_mode": "engine" | "assisted"` overrides the world's default memory mode.
- `budget.resolve_thinking_tokens` and `budget.narrate_thinking_tokens` let a thinking model (one whose chat template has `enable_thinking`, such as Qwen3.5 and later) reason before the resolve and narrate answers, up to that many tokens each; 0 turns a call's thinking off, otherwise the minimum is 64. A block that runs out is closed by the engine and the model answers from what it has. The reasoning never reaches the player or a later prompt; it is kept in the turn's telemetry and shown in the Turn log window. Both budgets are reserved in the context, like `resolve_max_tokens` and `narrate_max_tokens`. With a model that has no thinking mode, set both to 0.
- `gpu_layers` is how many of the chat model's layers go to the GPU (-1: all). A model larger than the GPU's memory needs a lower number: the rest runs on the CPU, slower. The example's 24 is a starting point for Qwen3.8-27B on a 10 GB GPU; raise it until loading fails for lack of memory, then step back.
- Qwen recommends temperature 1.0, top_k 20 and top_p 0.95 for its models in thinking mode, as in the example.

To check that the models, the Vulkan backend and KV-cache prefix reuse work, run the probe. It prints the prompt size and streams two completions; the second should report almost the whole prompt as reused:

```
./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
```

llama.cpp prints a few warnings while loading Qwen3 (control-token and `n_ctx_seq` notices); they are expected. The two runs' text can differ slightly even with the same seed. Qwen3.5 and later (Qwen3.8 among them) mix linear-attention layers into the model, whose cache llama.cpp cannot cut back to a shared prefix: with them every call evaluates its whole prompt, and the probe reports 0 reused.

Model-backed tests are tagged `[llama]` and skip unless both model variables are set (Git Bash, from the repo root). The thinking tests also need `MORPHEUS_TEST_THINKING_MODEL`, a thinking model: Qwen3.8 itself, or the small `Qwen3.5-0.8B-Q4_K_M.gguf` (`unsloth/Qwen3.5-0.8B-GGUF`, about 530 MB) for a quick run:

```
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
export MORPHEUS_TEST_THINKING_MODEL="$PWD/models/Qwen3.5-0.8B-Q4_K_M.gguf"
./build/dev/bin/Debug/morpheus_tests.exe "[llama]"
```

## Play in the app

The `dev` preset builds `morpheus`, the app. In Visual Studio, open `build/dev/MorpheusEngine.sln`: `morpheus` is the startup project and runs from the repository root, so F5 (or Ctrl+F5) starts it. Pick the RelWithDebInfo configuration to play: Debug is about 2-4x slower. From a shell, run it from the repository root, or pass the root folder:

```
./build/dev/bin/RelWithDebInfo/morpheus.exe [root]
```

The root holds `settings.json`, `game_projects/` and `saves/`. The models load in the background while the window opens. The windows dock and can be rearranged; the layout is saved in `imgui.ini` at the root (gitignored), and deleting it restores the default.

- **Runs:** the worlds in `game_projects/` and each world's runs, saved as `saves/<world id>/<name>.db`. Start a new run (a name, and a seed or blank for a random one) or continue a saved one.
- **Play:** the story, with each turn's rolls and changes in grey under your input. Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn.
- **State:** the world state, with what the latest turn changed highlighted.
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups and per-call token counts and timings, and the raw resolve output.
- **Context:** the latest resolve prompt by layer: tokens per layer, KV-cache reuse per call, and the text as sections or raw.
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores.
- **Settings:** model paths, context size, GPU layers, sampling and memory mode. Apply saves `settings.json` and restarts the engine, reopening the run; changing the models, the embedding prefixes, the context size or GPU layers reloads the models. The budget is shown and edited in `settings.json`.

`--frames N` exits after N frames, a quick check that the app starts on this machine.

## Play in the terminal

`morpheus_play` (`dev` preset) plays a world through the engine worker, the same path the app takes:

```
./build/dev/bin/Debug/morpheus_play.exe <settings.json> <world folder> <run.db> [--seed N] [--record <fixture.json>]
```

An existing run file is continued; otherwise a new run starts, seeded by `--seed` or randomly. At the prompt, type your action, or one of:

- `/undo`: take back the latest turn
- `/regen`: play the latest turn again with the same dice and new text
- `/state`: print the current state
- `/quit`: leave

`--record <file>` needs a new run file. When you quit, it writes a replay fixture of the session for `tests/data/replays`, where the replay test plays it back without a model. Commands that fail are left out of the fixture, together with their replies, except summaries: a summary recorded around a failed step stays in the fixture and never breaks a replay. If maintenance fails during the session no fixture is written. That guard covers maintenance failures reported before you quit: maintenance still running at `/quit` is cancelled by shutdown, and a summary call it cancels is not recorded at all. The fixture stores the world path exactly as you typed it, so run `--record` from the repo root with a repo-relative world path (for example `game_projects/sandcrawler`); a rooted or absolute path, or a world folder that does not exist from the current directory, is refused.

## Layout

- `src/core`: world loading, settings, schema-typed state, state changes, dice, turn record types
- `src/store`: one SQLite database per run
- `src/inference`: the `LlmBackend` interface (no llama.cpp)
- `src/context`: layered prompt assembly with exact token budgets
- `src/turn`: turn orchestration: grammar, resolve and apply, retrieval, folding, session, worker, replay
- `src/llama`: the llama.cpp backend (`dev` preset only)
- `tools/probe`: `morpheus_probe`, a manual model check (`dev` preset only)
- `tools/play`: `morpheus_play`, the terminal player (`dev` preset only)
- `src/app`: the app. `model/` is its state and actions (no SDL or ImGui), `ui/` its ImGui windows, `main.cpp` the SDL3 window (`dev` preset only)
- `tests/`: Catch2 tests and test data
- `game_projects/<id>/`: worlds as data
