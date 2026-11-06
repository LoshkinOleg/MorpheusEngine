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
- `narration_check` (true or false): after each narration a short review call checks it against the engine's outcome. A narration that contradicts it (a failed check told as a success, a change left out or reversed, a rejected change told as if it happened) is written once more and replaces the first.
- `compact_transcript` (true or false) and `prose_turns` (at least 1): the review also writes a one or two sentence digest of each turn, and in the prompt only the newest `prose_turns` turns keep their full narration; older ones show the digest. The model then copies its own prose less, and the transcript grows more slowly. The Play window always shows the full story.
- `budget.review_max_tokens` and `budget.review_thinking_tokens` are the review call's answer and thinking budgets (`review_max_tokens` at least 1, like the other answer budgets; `review_thinking_tokens` 0 or at least 64, like the other thinking budgets; together they must stay below `context_size`). The review prompt is short and standalone, so they are not reserved in the turn prompt's context.
- `gpu_layers` is how many of the chat model's layers go to the GPU (-1: all). A model larger than the GPU's memory needs a lower number: the rest runs on the CPU, slower. llama.cpp puts the last N layers on the GPU. Whatever else uses the GPU (a game, a browser decoding video) lowers the limit; see "Performance with Qwen3.8-27B on a 10 GB GPU" below for measured values.
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

### Performance with Qwen3.8-27B on a 10 GB GPU

Measured on 2026-11-05 with the Linux build below: RTX 3080 (10 GB), Ryzen 7 5800X (8 cores, 16 threads), 32 GB RAM, `Qwen3.8-27B-UD-Q4_K_M.gguf`, `context_size` 20576, measured with `morpheus_probe` on the sandcrawler world (a 738-token prompt).

| `gpu_layers` | Peak VRAM (desktop apps included) | Result |
|---|---|---|
| 30 | - | Fails: the 530 MB compute buffer does not fit, even with 8.6 GB free |
| 28 | 9822 of 10240 MiB | Loads, about 400 MB to spare; failed on a later run, so unreliable |
| 26 | 9415 of 10240 MiB | Loads with nothing else on the GPU; fails while a game holds 1-5 GB |

At 26 layers: loading both models takes 4-8 s (the model file cold or cached), the prompt is read at about 210 tokens/s, and output is written at about 2.5 tokens/s. A turn is two calls, each a thinking block and an answer, so with the example's thinking budgets (2048 resolve, 1536 narrate) a turn that uses both budgets takes about 25 minutes. Qwen3.8 thinks at maximum effort by default and tends to use its whole budget.

Known issue: `LlamaCppBackend` does not set `n_threads`, so llama.cpp computes the CPU-resident layers (about 60% of this model at 26 layers) with its default of 4 threads. Setting it to the machine's core count should speed up output; not measured yet.

Ways to faster turns, not done yet:

- Lower `resolve_thinking_tokens` and `narrate_thinking_tokens` (0 turns a call's thinking off).
- A mixture-of-experts model such as Qwen3.6-35B-A3B (about 3B parameters per token) with its expert tensors kept in RAM. This needs the backend to set `tensor_buft_overrides`; expected to write several times faster, not measured.

## Build (Linux)

Not the supported setup, but the `dev` targets build and run on Linux with clang and Ninja (tested on Ubuntu 24.04 in a distrobox on a Fedora-based host). Packages: `clang`, `cmake`, `ninja-build`, `pkg-config`, `libvulkan-dev`, `glslc`, `spirv-headers`, `libomp-dev`, `fonts-dejavu-core` (the app's font on Linux), and SDL3's X11 and Wayland development libraries (`libx11-dev`, `libxext-dev`, `libxrandr-dev`, `libxcursor-dev`, `libxi-dev`, `libxss-dev`, `libxfixes-dev`, `libxtst-dev`, `libxkbcommon-dev`, `libwayland-dev`, `wayland-protocols`, `libegl-dev`, `libgl-dev`, `libdrm-dev`, `libgbm-dev`, `libdecor-0-dev`, `libdbus-1-dev`, `libudev-dev`).

```
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMORPHEUS_BUILD_APP=ON -DMORPHEUS_BUILD_LLAMA=ON -DMORPHEUS_VULKAN=ON -DMORPHEUS_BUILD_TESTS=ON
cmake --build build/linux
./build/linux/bin/morpheus .
```

Inside a container, Vulkan sees only the CPU (llvmpipe) unless the host's NVIDIA user-space driver is visible: link the host's `libnvidia-*`, `libGLX_nvidia*`, `libEGL_nvidia*` and `libcuda*` libraries into a folder, put it on `LD_LIBRARY_PATH`, and point `VK_DRIVER_FILES` at an ICD file whose `library_path` is that folder's `libGLX_nvidia.so.0`. `vulkaninfo --summary` should then list the GPU. On Linux, the "SaveSettingsEdit reports a file it cannot replace" test fails: it relies on Windows refusing to replace a file another process holds open.

## Play in the app

The `dev` preset builds `morpheus`, the app. In Visual Studio, open `build/dev/MorpheusEngine.sln`: `morpheus` is the startup project and runs from the repository root, so F5 (or Ctrl+F5) starts it. Pick the RelWithDebInfo configuration to play: Debug is about 2-4x slower. From a shell, run it from the repository root, or pass the root folder:

```
./build/dev/bin/RelWithDebInfo/morpheus.exe [root]
```

The root holds `settings.json`, `game_projects/` and `saves/`. The models load in the background while the window opens. The windows dock and can be rearranged; the layout is saved in `imgui.ini` at the root (gitignored), and deleting it restores the default.

- **Runs:** the worlds in `game_projects/` and each world's runs, saved as `saves/<world id>/<name>.db`. Start a new run (a name, and a seed or blank for a random one) or continue a saved one.
- **Play:** the story, with each turn's rolls and changes in grey under your input. Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn. When the review finds that a narration contradicts the outcome, the text clears and the rewritten narration streams in its place.
- **State:** the world state, with what the latest turn changed highlighted.
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups, the narration review (each review's verdict, problems and digest, and the narration a rewrite replaced), per-call token counts and timings, and the raw resolve output.
- **Context:** the latest resolve prompt by layer: tokens per layer, KV-cache reuse per call, and the text as sections or raw.
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores and why each hit matched (`name`: named in your input; `scene`: named by the scene), and the scene values it searched with.
- **Settings:** the chat and embedding models, picked from the `.gguf` files in `models/`; context size, GPU layers, the resolve, narrate and review thinking budgets (0 for a model without a thinking mode), sampling, memory mode, the narration check, the compact transcript and its prose turns. Apply saves `settings.json` and restarts the engine, reopening the run; changing the models, the embedding prefixes, the context size or GPU layers reloads the models. The rest of the budget is shown and edited in `settings.json`.

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

When the review finds that a narration contradicts the outcome, the terminal prints `[narration rewritten: <problems>]` and streams the new narration below it.

`--record <file>` needs a new run file. When you quit, it writes a replay fixture of the session for `tests/data/replays`, where the replay test plays it back without a model. Commands that fail are left out of the fixture, together with their replies, except summaries: a summary recorded around a failed step stays in the fixture and never breaks a replay. If maintenance fails during the session no fixture is written. That guard covers maintenance failures reported before you quit: maintenance still running at `/quit` is cancelled by shutdown, and a summary call it cancels is not recorded at all. The fixture stores the world path exactly as you typed it, so run `--record` from the repo root with a repo-relative world path (for example `game_projects/sandcrawler`); a rooted or absolute path, or a world folder that does not exist from the current directory, is refused.

## Worlds

A world is a folder `game_projects/<id>/`: `world.json` (id, title, roll expression, check entity, stats, difficulties, default memory mode), `rules.md`, `lore.csv`, `state_schema.json` and `initial_state.json`. `world.json` may also declare a scene, the state values that say where the player is:

```json
"scene": {
  "paths": ["player.location", "crawler.location"],
  "present_at": "player.location"
}
```

Each path is `entity.field`, a string field of an entity in `initial_state.json`, and `present_at` is one of the paths. Every turn, retrieval also searches with those values and with the entities at `present_at`'s value (an NPC standing where the player stands, with its other string fields, such as its role), so lore about the place reaches the prompt even when the input names nothing. A world without a scene searches with the input and the previous turn's digest.

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
