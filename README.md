# Morpheus Engine

A local-first, LLM-driven text RPG engine in C++20. The engine owns the game (structured world state, dice, memory); a local model running in-process through llama.cpp proposes outcomes and narrates them.

The C# implementation this engine replaces was removed (it remains in the git history): `master` is the C++ engine.

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

llama.cpp's GPU backend is `MORPHEUS_GPU_BACKEND`: `Vulkan` (the default, used by `dev`), `CUDA` (needs the CUDA toolkit) or `CPU` (used by `dev-asan`). Any other value fails the configure.

## Models and settings

The `dev` and `linux-release` presets run models in-process through llama.cpp, or reach them through `llama-server` over HTTP (`backend`, below). For llama.cpp in-process, download two GGUF files into `models/` (gitignored):

- `Qwen3.8-27B-UD-Q4_K_M.gguf` (chat, a thinking model, about 16.5 GB) from Hugging Face `unsloth/Qwen3.8-27B-GGUF`
- `nomic-embed-text-v1.5.Q8_0.gguf` (embeddings, about 140 MB) from Hugging Face `nomic-ai/nomic-embed-text-v1.5-GGUF`

The model-backed tests also use `Qwen3-4B-Instruct-2507-Q4_K_M.gguf` (about 2.5 GB, `unsloth/Qwen3-4B-Instruct-2507-GGUF`), a model without a thinking mode, which also plays much faster on small GPUs.

Then copy `settings.example.json` (the template) to `settings.json` at the repository root and adjust it for your machine. `settings.json` is gitignored. Relative model paths resolve against the settings file's folder.

- `backend`: `"llama-cpp"` runs the models in-process and needs `chat_model`, `embedding_model` and `gpu_layers`; `"llama-server"` reaches them through two running `llama-server`s (see "Models through llama-server" below) and needs a `server` object instead (the model keys, if present, are ignored, as `server` is under `llama-cpp`):

  ```json
  "backend": "llama-server",
  "server": {
    "chat_url": "http://127.0.0.1:8080",
    "embedding_url": "http://127.0.0.1:8081",
    "slot": 0
  },
  ```

  The URLs are `http://host:port` (no HTTPS, no path, no IPv6 literal host; a bad URL fails at startup naming it); `slot` (at least 0) is the chat server's slot for Morpheus's calls. `context_size` is still what the budgets plan for: each of the chat server's slots must hold at least that many tokens, or the backend refuses to start.
- `budget.retrieval_cap` caps the retrieved memory (lore, past turns, notes) placed in the prompt, in tokens, including what `assisted`-mode lookups add. It is part of `budget.tail_allowance`.
- The optional top-level `"memory_mode": "engine" | "assisted"` overrides the world's default memory mode.
- `budget.resolve_thinking_tokens` and `budget.narrate_thinking_tokens` let a thinking model (one whose chat template has `enable_thinking`, such as Qwen3.5 and later) reason before the resolve and narrate answers, up to that many tokens each; 0 turns a call's thinking off, otherwise the minimum is 64. A block that runs out is closed by the engine and the model answers from what it has. The reasoning never reaches the player or a later prompt; it is kept in the turn's telemetry and shown in the Turn log window. Both budgets are reserved in the context, like `resolve_max_tokens` and `narrate_max_tokens`. With a model that has no thinking mode, set both to 0.
- `narration_check` (true or false): after each narration a short review call checks it against the engine's outcome. A narration that contradicts it (a failed check told as a success, a change left out or reversed, a rejected change told as if it happened) is written once more and replaces the first.
- `compact_transcript` (true or false) and `prose_turns` (at least 1): the review also writes a one or two sentence digest of each turn, and in the prompt only the newest `prose_turns` turns keep their full narration; older ones show the digest. The model then copies its own prose less, and the transcript grows more slowly. The Play window always shows the full story.
- `budget.review_max_tokens` and `budget.review_thinking_tokens` are the review call's answer and thinking budgets (`review_max_tokens` at least 1, like the other answer budgets; `review_thinking_tokens` 0 or at least 64, like the other thinking budgets; together they must stay below `context_size`). The review prompt is short and standalone, so they are not reserved in the turn prompt's context.
- `gpu_layers` is how many of the chat model's layers go to the GPU (-1: all). A model larger than the GPU's memory needs a lower number: the rest runs on the CPU, slower. llama.cpp puts the last N layers on the GPU. Whatever else uses the GPU (a game, a browser decoding video) lowers the limit; see "Performance with Qwen3.8-27B on a 10 GB GPU" below for measured values.
- Qwen recommends temperature 1.0, top_k 20 and top_p 0.95 for its models in thinking mode, as in the example.
- `min_p` (optional, 0 up to but not including 1, default 0) is a sampling entry's min-p cut: tokens less likely than `min_p` times the top token's probability are dropped. 0 means none.
- `thinking_format` (optional, default `"auto"`) says how a thinking model's block is written. A thinking block has five texts: `open` (the prompt's end when the call may think), `skip` (its end when it may not: an already-empty block, may be empty), `end` (the marker the model writes to end its thoughts), `after_end` (what follows `end` before the answer) and `close` (what the engine writes to close a block that ran out of budget: a nudge to answer now, then `end` and `after_end`). Its four forms: `"auto"` uses Qwen's texts when the chat template has the `enable_thinking` switch and no thinking otherwise; `"qwen"` uses Qwen's texts (`<think>`, `</think>`) whatever the template says; `"none"` means the model has no thinking block, so the engine writes none (every thinking budget must be 0); it is not a way to switch a thinking model's thinking off: for that keep `"auto"` (or `"qwen"`) and set every thinking budget to 0, and the engine writes the empty block; an object with exactly the keys `open`, `skip`, `end`, `after_end` and `close` (strings) is a format of its own, for a model family that spells the block differently. Checks at load: `open` and `end` must not be empty, and `close` must end with `end` followed by `after_end`. In-process (llama.cpp), `end` must also be a single token of the chat model, or loading fails: the engine recognises the end of the block by that token while generating. A call with a thinking budget on a model whose format resolved to none is refused. The app reloads its backend when the format changes.

To check that the models, the GPU backend and KV-cache prefix reuse work, run the probe. It prints the prompt size and streams two completions; the second should report almost the whole prompt as reused:

```
./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
```

llama.cpp prints a few warnings while loading Qwen3 (control-token and `n_ctx_seq` notices); they are expected. The two runs' text can differ slightly even with the same seed. Qwen3.5 and later (Qwen3.8 among them) mix linear-attention layers into the model, whose cache llama.cpp cannot cut back to a shared prefix: with them every call evaluates its whole prompt, and the probe reports 0 reused. That is in-process; llama-server keeps context checkpoints for such models, and what it reuses is measured below.

Model-backed tests are tagged `[llama]` and skip unless both model variables are set (Git Bash, from the repo root). The thinking tests also need `MORPHEUS_TEST_THINKING_MODEL`, a thinking model: Qwen3.8 itself, or the small `Qwen3.5-0.8B-Q4_K_M.gguf` (`unsloth/Qwen3.5-0.8B-GGUF`, about 530 MB) for a quick run:

```
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
export MORPHEUS_TEST_THINKING_MODEL="$PWD/models/Qwen3.5-0.8B-Q4_K_M.gguf"
./build/dev/bin/Debug/morpheus_tests.exe "[llama]"
```

### Models through llama-server

With `"backend": "llama-server"`, Morpheus runs no model itself: one model host can serve the app, `morpheus_play`, `morpheus_probe` and other programs at once, each on its own slot, and each slot keeps its own prompt cache. Morpheus does not build `llama-server`: use a llama.cpp release or your own build. Start a chat server and an embedding server, for example:

```
llama-server -m models/Qwen3.8-27B-UD-Q4_K_M.gguf --host 127.0.0.1 --port 8080 --parallel 3 --ctx-size 73728 -ngl 24
llama-server -m models/nomic-embed-text-v1.5.Q8_0.gguf --host 127.0.0.1 --port 8081 --embedding --ctx-size 2048 --batch-size 2048 --ubatch-size 2048
```

Use `127.0.0.1`, not `localhost`, in both the command lines and `settings.json`: on Windows a `localhost` connection tries IPv6 first and costs about 2 s per new connection.

- `--parallel N` gives the chat server N slots, each with `--ctx-size / N` tokens of context: make that at least `context_size` (73728 / 3 = 24576 above).
- The embedding server needs `--embedding` and a pooled sentence-embedding model; a batch as large as its context lets a long text be embedded in one piece, as in-process.
- Morpheus sends the prompt as token ids, tokenized as in-process (a player's text never becomes control tokens), and names the in-process sampling chain in every request (top-k, top-p, min-p, temperature; the server's other samplers neutral), so the server's command-line sampling options do not change Morpheus's calls.
- Thinking works as in-process: Morpheus writes the thinking block's texts itself (from `thinking_format`), so the thinking budgets apply unchanged.
- Connections: Morpheus keeps one connection alive to each server (a second, dedicated one for the streamed `/completion`, so a cancel closes only that one). llama-server closes a connection idle for 5 s, so a non-streaming request, and a `/completion` that got no answer, is sent once more when a stale connection failed it.
- The app keeps its backend while the settings are unchanged, and the backend reads the servers' facts (the thinking format's texts, the BOS token, the embedding model's id) once at startup, so after swapping a server's model under the same URL, reload the backend (change and restore a setting, or restart the app): otherwise new embeddings are stored under the old embedding model's id.
- `morpheus_probe` works with either backend; with llama-server its reuse figures are the server's, so the second run shows what a slot reuses.
- With a model whose layers are partly linear attention (Qwen3.5 and later), add `--checkpoint-min-step 0`: llama-server can only resume such a model from a saved checkpoint, and by default it saves one at most every 8192 tokens, so a slot reuses little more than its first prompt (see the figures below).
- To build `llama-server` from the llama.cpp that the `dev` preset fetches (same pinned commit):

  ```
  cmake -S build/dev/_deps/llama-src -B build/llama-server -DGGML_VULKAN=ON -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_OPENSSL=OFF
  cmake --build build/llama-server --config Release --target llama-server
  ```

### The [server] tests

Tests tagged `[server]` run against your servers and skip unless both variables are set. They need a chat server with at least 3 slots of at least 16384 tokens (`--parallel 3 --ctx-size 49152`):

```
export MORPHEUS_TEST_SERVER_URL=http://127.0.0.1:8080
export MORPHEUS_TEST_EMBED_SERVER_URL=http://127.0.0.1:8081
./build/tests/bin/Debug/morpheus_tests.exe "[server]"
```

They complete a resolve call under its grammar and a streamed narrate call, think on a thinking model (skipped on one without a thinking mode), and run three sessions on three slots at once, each alternating an action call and a speech call for ten exchanges; that test prints each call's reused share of its prompt (`tokensReused / promptTokens`) and checks that every call after a session's first reuses some. In the `dev` build, with the `[llama]` variables set too, `"[server][llama]"` checks that the server and the in-process backend count the same tokens when the server runs the `MORPHEUS_TEST_CHAT_MODEL` file. `morpheus_server`'s own `[server]` tests play the conversation fixture over HTTP against your servers and print each turn's time to its first sentence, and play the three-actor memory fixture (`tests/hosted/fixtures/three_actors.json`) over HTTP, checking only what cannot depend on the model's words: nobody remembers what it did not witness. The `"[llama][memory]"` test plays the same fixture in-process on the `MORPHEUS_TEST_CHAT_MODEL` file.

### Prompt-cache reuse through llama-server

R2 of the Skyrim integration asks for more than 80% of the prompt reused on the second and later calls of a session. Measured on 2026-11-14 with the three-slot `[server]` test (three sessions at once, ten exchanges of an action call and a speech call each). Windows, RTX 3070 Laptop GPU (8 GB), `llama-server` built from the pinned llama.cpp with Vulkan, chat server started with `--parallel 3 --ctx-size 49152 -ngl 99 -fa on -ctk q8_0 -ctv q8_0`:

| Chat model | Server options | Reused share, calls 2-20 | Mean per session |
|---|---|---|---|
| `Qwen3-4B-Instruct-2507-Q4_K_M` | as above | 0.97 to 1.00 | 0.99 |
| `Qwen3.5-9B-Q4_K_M` | as above | falls from about 0.78 to about 0.65 as the session grows | 0.70 to 0.72 |
| `Qwen3.5-9B-Q4_K_M` | plus `--checkpoint-min-step 0` | rises from about 0.78 to 0.82 as the session grows | 0.79 to 0.80 |

- Qwen3-4B-Instruct has only full-attention layers, so a slot reuses everything up to the first token that differs.
- Qwen3.5 resumes only from a saved checkpoint. With the default spacing each slot keeps one checkpoint near its first prompt (about 1,900 tokens), so the reused share shrinks as the history grows. With `--checkpoint-min-step 0` a checkpoint is saved after every prompt; what is left to recompute is the previous answer and the new text, so the share grows with the history and reaches 80% from about the tenth call.
- The thinking test and the `"[server][llama]"` agreement test (with Qwen3-4B-Instruct) passed on the same machine.

### Performance with Qwen3.8-27B on a 10 GB GPU

Measured on 2026-11-05 with a Linux build, llama.cpp on Vulkan: RTX 3080 (10 GB), Ryzen 7 5800X (8 cores, 16 threads), 32 GB RAM, `Qwen3.8-27B-UD-Q4_K_M.gguf`, `context_size` 20576, measured with `morpheus_probe` on the sandcrawler world (a 738-token prompt).

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

Status: an earlier Vulkan build of llama.cpp was tested in an Ubuntu 24.04 distrobox; these presets have not been run yet. On Linux, `cmake --list-presets` shows two presets (and on Windows only the Windows ones):

| Preset | Builds |
|---|---|
| `linux-tests` | Engine libraries and tests only (no llama.cpp or SDL3), Debug |
| `linux-release` | Everything, Release: llama.cpp with CUDA for the GPU in the machine (`MORPHEUS_GPU_BACKEND` `CUDA`, `CMAKE_CUDA_ARCHITECTURES` `native`), the `morpheus` app, `morpheus_probe`, `morpheus_play` and the tests |

Packages for `linux-tests`: `clang`, `cmake`, `ninja-build`, `git`.

```
cmake --preset linux-tests
cmake --build --preset linux-tests
ctest --preset linux-tests
```

`linux-release` also needs:

- the CUDA toolkit: NVIDIA's `cuda-toolkit` package from its Ubuntu 24.04 repository, with `nvcc` on `PATH` (usually `/usr/local/cuda/bin`), the NVIDIA driver, and `g++` (`nvcc` compiles CUDA host code with it);
- `pkg-config`, `libomp-dev`, `fonts-dejavu-core` (the app's font on Linux);
- SDL3's X11 and Wayland development libraries: `libx11-dev`, `libxext-dev`, `libxrandr-dev`, `libxcursor-dev`, `libxi-dev`, `libxss-dev`, `libxfixes-dev`, `libxtst-dev`, `libxkbcommon-dev`, `libwayland-dev`, `wayland-protocols`, `libegl-dev`, `libgl-dev`, `libdrm-dev`, `libgbm-dev`, `libdecor-0-dev`, `libdbus-1-dev`, `libudev-dev`.

```
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/bin/morpheus .
```

`CMAKE_CUDA_ARCHITECTURES` `native` needs a GPU visible at configure time. Where none is (a distrobox without NVIDIA passthrough, say), name the architecture, here 86 for an RTX 30 series card: `cmake --preset linux-release -DCMAKE_CUDA_ARCHITECTURES=86`.

To run llama.cpp on Vulkan instead, configure with `cmake --preset linux-release -DMORPHEUS_GPU_BACKEND=Vulkan` and install `libvulkan-dev`, `glslc` and `spirv-headers` too. Inside a container, Vulkan sees only the CPU (llvmpipe) unless the host's NVIDIA user-space driver is visible: link the host's `libnvidia-*`, `libGLX_nvidia*`, `libEGL_nvidia*` and `libcuda*` libraries into a folder, put it on `LD_LIBRARY_PATH`, and point `VK_DRIVER_FILES` at an ICD file whose `library_path` is that folder's `libGLX_nvidia.so.0`. `vulkaninfo --summary` should then list the GPU.

## Using Morpheus from another project

Another CMake project can build the engine into itself with FetchContent and link the namespaced targets:

```cmake
include(FetchContent)
FetchContent_Declare(
    morpheus
    GIT_REPOSITORY https://github.com/LoshkinOleg/MorpheusEngine.git
    GIT_TAG        <commit SHA>
    GIT_SHALLOW    FALSE
    SYSTEM
)
FetchContent_MakeAvailable(morpheus)

target_link_libraries(my_target PRIVATE Morpheus::turn)
```

- Included this way (not the top-level project), Morpheus builds only its engine libraries, `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn` and `Morpheus::llamaserver` (the llama-server backend, built while `MORPHEUS_BUILD_LLAMA_SERVER` is on), with nlohmann/json, SQLite (as `morpheus_sqlite3`, so it never meets a `sqlite3` target of yours) and cpp-httplib (as `morpheus_httplib`, header-only and without OpenSSL; fetched while `MORPHEUS_BUILD_LLAMA_SERVER` is on, its default, also when vendored). `MORPHEUS_BUILD_APP`, `MORPHEUS_BUILD_TESTS` and `MORPHEUS_BUILD_LLAMA` default to off, so no llama.cpp, SDL3, ImGui or Catch2 is fetched.
- Link the `Morpheus::` names, not the internal `morpheus_*` ones. For the llama.cpp backend, `Morpheus::llama`, `set(MORPHEUS_BUILD_LLAMA ON)` (and `MORPHEUS_GPU_BACKEND`: `Vulkan`, `CUDA` or `CPU`) before `FetchContent_MakeAvailable`. `Morpheus::app_model` and `Morpheus::app_ui` exist when the app or the tests are built.
- A project that already provides `nlohmann_json::nlohmann_json` (its own FetchContent or `find_package`) keeps its copy, and Morpheus links the same target. Make yours available BEFORE `FetchContent_MakeAvailable(morpheus)`: Morpheus checks for the target when it is included, so the guard only works in that order.
- `SYSTEM` makes Morpheus's headers count as system headers in your own files, so your warnings do not fire on them.
- Morpheus's warnings stay on but are not errors (`MORPHEUS_WARNINGS_AS_ERRORS` defaults to off when vendored), so a warning from your compiler does not fail your build. `MORPHEUS_WARNINGS_AS_ERRORS` uses CMake's `COMPILE_WARNING_AS_ERROR` property, so it holds even when you set `CMAKE_COMPILE_WARNING_AS_ERROR`; a global `add_compile_options(-Werror)` of yours still reaches Morpheus's targets. Output directories and the Visual Studio startup project are yours.

`cmake/consumer-check/` is such a project, built from this checkout (`-DMORPHEUS_SOURCE_DIR=<path>` picks another): it fails to configure if the vendored build defines anything beyond the engine libraries and `morpheus_httplib`, and its program calls into `core` and `turn`. On Linux:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G Ninja && cmake --build build/consumer-check && ./build/consumer-check/consumer_check
```

On Windows (Git Bash), the same with the Visual Studio generator:

```bash
cmake -S cmake/consumer-check -B build/consumer-check -G "Visual Studio 17 2022" -A x64 && cmake --build build/consumer-check && ./build/consumer-check/Debug/consumer_check.exe
```

It prints `consumer_check: ok` and exits with 0.

## Play in the app

The `dev` preset (on Linux, `linux-release`) builds `morpheus`, the app. In Visual Studio, open `build/dev/MorpheusEngine.sln`: `morpheus` is the startup project and runs from the repository root, so F5 (or Ctrl+F5) starts it. Pick the RelWithDebInfo configuration to play: Debug is about 2-4x slower. From a shell, run it from the repository root, or pass the root folder:

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
- **Settings:** the backend; under llama.cpp the chat and embedding models, picked from the `.gguf` files in `models/`, and GPU layers; under llama-server the two server URLs and the slot; context size, the resolve, narrate and review thinking budgets (0 for a model without a thinking mode), sampling, memory mode, the narration check, the compact transcript and its prose turns. Apply saves `settings.json` and restarts the engine, reopening the run; changing the backend, the models, the server URLs or slot, the embedding prefixes, the context size or GPU layers reloads the backend. The rest of the budget is shown and edited in `settings.json`.
- **Model seat:** a debug window for answering the engine's model calls yourself. Tick "Answer as the model" and choose, per call (resolve, narrate, rewrite, lookup, review, summarize, condense), Human (you write the reply), Draft (the model answers first and you submit or edit its answer) or Model (as usual); every call starts in Human. A waiting call shows its token limits, its prompt by layer, its grammar and the model's draft. Check counts the reply's tokens and tests it against the grammar; Submit sends it whether or not it fits, and the engine treats it as the model's reply, mistakes included; Withdraw releases the call without a reply (during a turn it cancels the turn, as Cancel does; between turns it ends that fold or condense, and the next turn folds what it needs); Check is disabled for a call without a grammar. A summary that a cancelled turn interrupted comes back between turns, and can be withdrawn again. Nothing is saved: the seat is off whenever the app starts.

`--frames N` exits after N frames, a quick check that the app starts on this machine.

## Play in the terminal

`morpheus_play` (`dev` and `linux-release` presets) plays a world through the engine worker, the same path the app takes:

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

## morpheus_server (hosted runs over HTTP)

`morpheus_server` is a localhost HTTP server through which a host (the Skyrim/Mantella fork) plays hosted runs: it keeps the runs, plays each turn through the engine and streams the action and the spoken line back as Server-Sent Events. Its design is in `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, hosted runs in `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, and the host data and request format in `tests/hosted/fixtures/README.md`.

It builds in every top-level preset (`MORPHEUS_BUILD_SERVER`, default on when top level), with no llama.cpp. The binary is `build/<preset>/bin/<config>/morpheus_server(.exe)`: `build/tests/bin/Debug/` in the `tests` preset, `build/dev/bin/<config>/` in `dev`.

To run it, start the two llama-servers as in "Models through llama-server", the chat server with `--parallel 3` or more (slot 0 is for folding, the others for conversations), and set `"backend": "llama_server"` in settings.json (its `server.slot` is not used: the server hands out slots itself). Then:

```
./build/dev/bin/Release/morpheus_server --port 4998 --runs runs --settings settings.json
```

For hosted dialogue set `budget.resolve_thinking_tokens` and `budget.narrate_thinking_tokens` to 0: a thinking block delays the first spoken sentence, which a game's dialogue cannot wait for.

The flags and their defaults are `--port 4998`, `--runs ./runs` and `--settings ./settings.json`. The server listens on 127.0.0.1 only and owns its port alone: a port already in use ends it at startup with a message. A run's file is `<runs>/<run_id>.morpheus`. Ctrl+C stops it; turns still running commit nothing, and their streams end with `error` 503 ("the server is stopping"). A `run_id` is 1 to 64 characters of `A-Z a-z 0-9 _ -` and not a Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`, in any case); one that differs from an opened run's only in case is refused with 400, and so is any `open` once shutdown has begun (503).

Every response carries `"api_version": "1.4.0"` (the changes are in `CHANGELOG.md`), error answers the HTTP layer makes itself (OPTIONS, an unknown method, a malformed request) included. The endpoints:

| Endpoint | Body | Answer |
|---|---|---|
| `GET /health` | - | `{ready, chat_model, embedding_model, slots}` |
| `GET /v1/prompts/default` | - | `{prompts, variables}`: the default texts and their variables; added in 1.3.0 (F) |
| `POST /v1/runs/{run_id}/open` | `{catalog, observation_schema, lore?, clock?, prompts?, seed?}` | `{run_id, created}`; the same host data reopens the run as it is |
| `POST /v1/runs/{run_id}/conversations` | `{participants, observation}` | `{conversation_id}` |
| `POST /v1/runs/{run_id}/conversations/{cid}/turn` | `{speaker, player_text?, events?, observation, allowed_actions?}` (`speaker` is an NPC id or `"decide"`, added in 1.4.0 (G1)) | a Server-Sent Events stream |
| `POST /v1/runs/{run_id}/conversations/{cid}/events` | `{events}` | `{}` |
| `POST /v1/runs/{run_id}/conversations/{cid}/participants` | `{joined?, left?, observation}` | `{participants}` |
| `POST /v1/runs/{run_id}/conversations/{cid}/end` | `{}` | `{}`; folding follows in the background |
| `GET /v1/runs/{run_id}/conversations/{cid}` | - | `{conversation, turns, events}`: the stored records |
| `POST /v1/runs/{run_id}/memories/share` | `{from, to, conversation_id}` | `{shared, pending}`; added in 1.1.0: `to` learns what `from` remembers of the conversation |
| `GET /v1/runs/{run_id}/actors/{actor_id}/memories` | - | `{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}`, oldest first; added in 1.1.0 |
| `POST /v1/runs/{run_id}/rewind` | `{game_time}` | `{rewound, hidden_turns, restored_turns, rollback}`; added in 1.2.0: the run as it was at that game time |
| `POST /v1/tokenize` | `{content}` | `{tokens}`: a token count |
| `POST /v1/embeddings` | `{input, purpose}` | `{embeddings, model}` |

A turn's stream has these events, in order: `action` (the adjudicated action, sent before the line starts; when the proposal was refused it is `"action": null` plus a separate `"refused": [reasons]` key), `token` (one per streamed piece), `sentence` (each complete sentence, as soon as it is complete), then `done` (the speaker, action, line, turn number, `events_omitted`, timings) or `error` (`status` and `message`, when a failure comes after the stream has started). A client that disconnects stops the turn and nothing is committed. Events recorded since a conversation's last turn that do not fit its tail leave the prompt, oldest first; `events_omitted` says how many, and the store keeps them all.

**Memory.** Each NPC remembers only what it witnessed: the turns it spoke in or heard (it was a participant when they were played). Its memories are summaries written in its own words (the fold names the NPC), shown in the tail of its prompt under `# What <name> remembers`, and retrieval for its turns searches only what it saw and was told. Nobody else's summaries reach its prompt: an NPC that left the room does not know what was said after. `POST /v1/runs/{run_id}/memories/share` lets one NPC tell another what it remembers of a conversation: the listener gets copies marked as told by the teller, which it then keeps, retrieves and condenses like its own. A share of a conversation that has not ended, or whose memory for the teller is not complete yet, waits (`pending: true`) and lands when the teller's memory is complete; `shared` counts the copies made now. A share to the player is refused: the player has no memories in Morpheus (the host's player is a person). `GET .../actors/{actor_id}/memories` shows an NPC's memories, the ones condensed away (`superseded`) included. Design: `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`.

**Rewind.** When a player loads an older save, the host calls `POST /v1/runs/{run_id}/rewind` with the save's game time, on every save load (a load of the latest save changes nothing: the call is idempotent). It needs the host data's `clock`, an optional JSON pointer (RFC 6901) to a number in every observation, the game time; every observation then must hold a finite number there, and a rewind of a run whose host data names no clock is refused with 400. Rewinding hides, and deletes nothing: the turns, events, participant changes, conversation ends and shares clocked after that time, and the conversations started after it, leave every prompt, retrieval and memory listing, and an NPC's memory that covers a hidden turn is hidden and written again, in the background, from the turns that remain. A later rewind to a later time brings it all back (roll-forward), exactly as it was, for as long as nothing new has been recorded. The first new record after a rewind (a turn, events, a conversation, a participant change, an end or a share) abandons the hidden future for good: a rewind to the old time then restores nothing. `rewound` says whether the call changed what is hidden, and `hidden_turns` and `restored_turns` are the net counts of turns it hid and showed again (a turn shown by the undo and hidden again by the new rollback counts as neither). `rollback` in the answer is `{"game_time": T}` of the rollback left open after the call, or null when nothing is hidden. Design: `docs/superpowers/specs/2026-11-16-rollback-design.md`.

**Prompt pack.** The host data may carry `prompts`, an object of texts that replace the engine's defaults for the run. The seven keys are `system` (the system text, no variable), `resolve` (the instruction that asks for the speaker's action), `speak` (the instruction that asks for the spoken words) and `turn_cue` (the user message shown for a turn with no events and no player text, before the speaker's line; each taking `{speaker}` and `{player}`), `decide` (the instruction of the call that chooses the speaker of a `"decide"` turn, taking `{player}`), and `summarize` and `condense` (the instructions that fold a conversation into an NPC's memory and condense memories, each taking `{actor}`). A key left out keeps the engine's default; `GET /v1/prompts/default` answers every default text and the variables it takes. A variable is `{` then lower-case letters and underscores, then `}`; every other brace is literal text. An unknown key, an empty text, a variable the text does not take, or a system text over `engine_rules_cap` fails the `open` with 400 naming `prompts/<key>`. A changed pack is changed host data: the run's next prompt uses it. Design: `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`.

**Decide.** A turn whose `speaker` is `"decide"` asks the engine to choose who speaks (added in 1.4.0). The candidates are the conversation's NPCs that are in the turn's observation, in join order; the player is never one. On a turn with no player text and no new events (recorded since the last turn or sent with it) the last speaker is left out while another candidate remains, so two NPCs with no input alternate. With one candidate it is chosen at once, with no model call; with more, a short grammar-constrained model call chooses among them. No NPC in the observation is a 400, and an observation actor with the id `decide` is a 400 (the id is reserved). The `action` event's `speaker` names the NPC chosen, and the stored turn's speaker is that NPC. When the model's answer is refused, the candidate who spoke least recently is chosen instead; a decided turn's `timings`, in the `done` event and in the stored turn, gain `decide` (the call's telemetry, with `refused` holding the reasons in that case) only when a call ran. Design: `docs/superpowers/specs/2026-11-16-decide-speaker-design.md`.

A run keeps its NPCs' memories in the background, between requests: it folds its ended conversations and catches up every NPC who is behind. A turn stops a running fold at once (it resumes later); every other request to that run waits only for the current step (one NPC's memory of one conversation, or one share), after which the background work pauses and resumes once the queue is empty.

An example session, in Git Bash or on Linux, with the host data and the observation from the fixtures:

```
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/open -H "Content-Type: application/json" --data @tests/hosted/fixtures/open.json
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations -H "Content-Type: application/json" \
  --data '{"participants": ["0001A6A4", "00000007"], "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations/c1/events -H "Content-Type: application/json" \
  --data '{"events": [{"type": "heard", "text": "A dragon roars in the distance."}]}'
curl -N -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations/c1/turn -H "Content-Type: application/json" \
  --data '{"speaker": "0001A6A4", "player_text": "Can you follow me?", "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations/c1/end -H "Content-Type: application/json" --data '{}'
```

The turn's stream looks like this:

```
event: action
data: {"action":{"id":"follow","params":{}},"api_version":"1.4.0","speaker":"0001A6A4"}

event: token
data: {"text":"Lead"}
...
event: sentence
data: {"text":"Lead the way, then."}

event: done
data: {"action":{"id":"follow","params":{}},"events_omitted":0,"line":"Lead the way, then.","speaker":"0001A6A4","timings":{...},"turn":1}
```

Errors answer `{"api_version", "error": {"status", "message"}}` with these statuses:

- 400: a malformed body or field (the message names it), a bad `run_id`, no NPC participant in the observation for a `"decide"` turn, or a lore entry the embedding server refuses (too long for its context)
- 404: an unknown route, or an unknown conversation of an opened run
- 409: a run not opened in this server process
- 422: the observation and player text do not fit the tail
- 503: a llama-server is unreachable or failed, also while it embeds lore at `open`; try again later
- 500: anything else, logged with its message

The `"[server]"` test (see "The [server] tests") also plays the conversation fixture over HTTP against your servers and prints each turn's time to its first sentence. Measured on 2026-11-14 on Windows with an RTX 3070 Laptop GPU (8 GB; llama-server with Vulkan, Qwen3-4B-Instruct-2507-Q4_K_M with --parallel 3 --ctx-size 49152 -fa on -ctk q8_0 -ctv q8_0, nomic-embed for embeddings): the four turns of conversation.json reached their first sentence after 955, 2522, 1745 and 709 ms. With Qwen3.5-9B-Q4_K_M on the same GPU (plus `--checkpoint-min-step 0`), measured on 2026-11-15: 1750, 2371, 1629 and 2088 ms. The host's 1.5 s target is for its RTX 3060 Ti with that 9B model.

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
- `src/inference`: the `Tokenizer`, `ChatModel` and `Embedder` interfaces and `LlmBackend`, their union (no llama.cpp)
- `src/context`: layered prompt assembly with exact token budgets
- `src/grammar`: bounded-JSON grammars: the GBNF builder, `FromSchema`, `Validate` and `grammar::Check` (no llama.cpp)
- `src/turn`: turn orchestration: grammar, resolve and apply, retrieval, folding, session, worker, replay
- `src/llama`: the llama.cpp backend (`dev` and `linux-release` presets only)
- `src/llamaserver`: the llama-server backend over HTTP (cpp-httplib; every preset, `MORPHEUS_BUILD_LLAMA_SERVER`)
- `src/server`: `morpheus_server`'s library: run workers, slots, the transport-free API and the HTTP adapter (cpp-httplib; every top-level preset, `MORPHEUS_BUILD_SERVER`)
- `tools/server`: `morpheus_server`, the HTTP server for hosted runs (every top-level preset)
- `tools/probe`: `morpheus_probe`, a manual model check (`dev` and `linux-release` presets only)
- `tools/play`: `morpheus_play`, the terminal player (`dev` and `linux-release` presets only)
- `src/app`: the app. `model/` is its state and actions (no SDL or ImGui), `ui/` its ImGui windows, `main.cpp` the SDL3 window (`dev` and `linux-release` presets only)
- `tests/`: Catch2 tests and test data
- `cmake/consumer-check/`: a project that includes Morpheus as another project would (see "Using Morpheus from another project")
- `game_projects/<id>/`: worlds as data
