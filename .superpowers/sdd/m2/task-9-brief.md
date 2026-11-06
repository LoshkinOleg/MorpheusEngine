### Task 9: Probe tool, docs, milestone verification

**Files:**
- Create: `tools/probe/CMakeLists.txt`, `tools/probe/main.cpp`
- Modify: root `CMakeLists.txt` (`add_subdirectory(tools/probe)` inside `if(MORPHEUS_BUILD_LLAMA)`), `CLAUDE.md`, `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

**Interfaces:**
- Consumes: `LoadSettings`, `LoadWorld`, `LlamaCppBackend`, `AssembleResolvePrompt`.
- Produces: the `morpheus_probe` executable (dev preset only).

- [ ] **Step 1: Create `tools/probe/main.cpp`**

```cpp
// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the Vulkan backend and
// prefix caching work on this machine.
//
// Usage: morpheus_probe <settings.json> <world folder> "<player input>"

#include <exception>
#include <format>
#include <iostream>
#include <string_view>

#include "context/PromptBuilder.h"
#include "core/Settings.h"
#include "core/World.h"
#include "llama/LlamaCppBackend.h"

using namespace morpheus;

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: morpheus_probe <settings.json> <world folder> \"<player input>\"\n";
        return 2;
    }
    try {
        const core::Settings settings = core::LoadSettings(argv[1]);
        const core::World world = core::LoadWorld(argv[2]);

        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend backend(config);

        // A plain-prose stand-in for the resolve call: the real engine rules and grammar arrive in milestone 3.
        context::ContextInputs inputs;
        inputs.engineRules = "You are the game master of a solo text RPG. Reply in plain prose.";
        inputs.state = world.initialState;
        inputs.playerInput = argv[3];
        inputs.instruction = "Narrate what happens next in two or three sentences.";
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, settings.budget, backend);
        std::cout << std::format("prompt: {} tokens (engine rules {}, world {}, tail {}); fits: {}\n",
                                 assembly.tokens.total, assembly.tokens.engineRules, assembly.tokens.world,
                                 assembly.tokens.tail, assembly.Fits() ? "yes" : "no");

        inference::GenerationSpec spec;
        spec.maxTokens = settings.budget.narrateMaxTokens;
        spec.sampling.temperature = static_cast<float>(settings.narrateSampling.temperature);
        spec.sampling.topK = settings.narrateSampling.topK;
        spec.sampling.topP = static_cast<float>(settings.narrateSampling.topP);
        spec.sampling.seed = 1234;

        // The second run should reuse almost the whole prompt from the KV cache.
        for (int run = 1; run <= 2; ++run) {
            std::cout << "\n--- run " << run << " ---\n";
            const inference::Completion completion = backend.Complete(
                assembly.prompt, spec, [](std::string_view piece) { std::cout << piece << std::flush; }, {});
            std::cout << std::format("\n[prompt {} tokens: {} reused, {} evaluated in {:.0f} ms; {} generated in {:.0f} ms]\n",
                                     completion.promptTokens, completion.tokensReused, completion.tokensEvaluated,
                                     completion.promptMs, completion.tokensGenerated, completion.generateMs);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
```

`tools/probe/CMakeLists.txt`:

```cmake
add_executable(morpheus_probe main.cpp)
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_llama)
morpheus_target_defaults(morpheus_probe)
```

In the root `CMakeLists.txt`, extend the `if(MORPHEUS_BUILD_LLAMA)` block that adds `src/llama` with `add_subdirectory(tools/probe)`. Because `src/context` must exist before the probe, that block has to come after `add_subdirectory(src/context)`; move it there if needed.

- [ ] **Step 2: Update the docs**

**`CLAUDE.md`:**
- **Layout:** add
  - `src/inference` (the LlmBackend interface, no llama.cpp)
  - `src/context` (prompt assembly)
  - `src/llama` (llama.cpp backend, dev preset only)
  - `tools/probe`
  - `models/` and `settings.json` (both gitignored; see `settings.example.json`)
- **Build:**
  - `tests` and `asan` build without llama.cpp;
  - `dev` builds llama.cpp with Vulkan;
  - model-backed tests are tagged `[llama]` and need `MORPHEUS_TEST_CHAT_MODEL` and `MORPHEUS_TEST_EMBED_MODEL`.

**`README.md`:**
- **Prerequisites:** add the Vulkan SDK (for the `dev` preset).
- **Presets table:** update it.
- **Models:** add a short "Models and settings" section listing the two model downloads, how to copy `settings.example.json` to `settings.json`, and how to run the probe:

  ```
  ./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
  ```

**Architecture spec:**
- In the `turns` table row, `rolls` becomes `checks` ("typed check outcomes").
- In "Inference interface", note:
  - `save_state`/`restore_state` carry the cached token list;
  - the llama.cpp backend is built with Vulkan;
  - grammar text is supplied by the caller (generated from the schema in M3).
- In the module table, `core` also holds settings and turn-record types.

- [ ] **Step 3: Verify the milestone**

Run each preset:
1. `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`. Expected: all pass, and llama.cpp is not configured.
2. `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`. Expected: all pass, with no AddressSanitizer report.
3. `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`. Expected:
   - all non-model tests pass;
   - `[llama]` tests are skipped, or pass when the model env vars are set;
   - `morpheus_probe.exe` is built.

If the model files exist under `models/` and a `settings.json` exists, also run the probe command from the README and record:
- the token line from each run;
- that run 2 reports `reused` = prompt tokens - 1.

If they do not exist, say so; that is not a failure.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tools/probe CLAUDE.md README.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Add morpheus_probe and document milestone 2" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```
