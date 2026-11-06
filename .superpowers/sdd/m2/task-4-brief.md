### Task 4: Inference interface, Vulkan llama.cpp build, test fake

**Files:**
- Modify: `CMakeLists.txt`, `CMakePresets.json`, `tests/CMakeLists.txt`
- Create: `src/inference/CMakeLists.txt`, `src/inference/Llm.h`, `src/inference/Llm.cpp`, `tests/fakes/FakeBackend.h`, `tests/inference/LlmTests.cpp`

**Interfaces:**
- Consumes: `core::ContractError`.
- Produces (namespace `morpheus::inference`):
  - `enum class Role { System, User, Assistant }` and `std::string RoleName(Role)`, which returns `"system"`, `"user"` or `"assistant"`
  - `struct Message { Role role; std::string content; }`
  - `struct Prompt { std::vector<Message> messages; std::size_t stableCount; }`
  - `struct Sampling { float temperature; int topK; float topP; std::uint32_t seed; }`
  - `struct GenerationSpec { std::string grammar; int maxTokens; Sampling sampling; }`
  - `struct Completion { std::string text; int promptTokens, tokensReused, tokensEvaluated, tokensGenerated; bool stoppedAtLimit, cancelled; double promptMs, generateMs; }`
  - `enum class EmbedPurpose { Query, Document }`
  - `using TokenCallback = std::function<void(std::string_view)>`
  - `class InferenceError : std::runtime_error`
  - `class LlmBackend`, with pure virtual methods:
    - `int CountTokens(std::string_view)`
    - `int CountPromptTokens(const Prompt&)`
    - `int ContextSize() const`
    - `Completion Complete(const Prompt&, const GenerationSpec&, const TokenCallback&, std::stop_token)`
    - `std::vector<std::vector<float>> Embed(const std::vector<std::string>&, EmbedPurpose)`
    - `std::string EmbeddingModelId() const`
    - `std::vector<std::uint8_t> SaveState()`
    - `void RestoreState(std::span<const std::uint8_t>)`
  - Test double `morpheus::test::FakeBackend(int contextSize)`:
    - `CountTokens` = bytes / 4, rounded up.
    - `CountPromptTokens` = the sum, over messages, of the content's tokens + 4.
    - Every other method throws `ContractError`.
- CMake:
  - option `MORPHEUS_BUILD_LLAMA` (default ON) and option `MORPHEUS_VULKAN` (default ON);
  - targets `morpheus_inference` and (later) `morpheus_llama`.

- [ ] **Step 1: Write the failing test `tests/inference/LlmTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "inference/Llm.h"

using namespace morpheus::inference;

TEST_CASE("RoleName spells roles as chat templates expect", "[inference]")
{
    CHECK(RoleName(Role::System) == "system");
    CHECK(RoleName(Role::User) == "user");
    CHECK(RoleName(Role::Assistant) == "assistant");
}

TEST_CASE("FakeBackend counts tokens deterministically through the interface", "[inference]")
{
    morpheus::test::FakeBackend fake(1000);
    LlmBackend& backend = fake;

    CHECK(backend.CountTokens("") == 0);
    CHECK(backend.CountTokens("abcd") == 1);
    CHECK(backend.CountTokens("abcde") == 2);

    Prompt prompt;
    prompt.messages = {{Role::System, "abcd"}, {Role::User, "abcdefgh"}};
    CHECK(backend.CountPromptTokens(prompt) == (1 + 4) + (2 + 4));
    CHECK(backend.ContextSize() == 1000);
    CHECK_THROWS_AS(backend.Complete(prompt, GenerationSpec{}, {}, {}), morpheus::core::ContractError);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'fakes/FakeBackend.h'` (or `'inference/Llm.h'`).

- [ ] **Step 3: Create `src/inference/Llm.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::inference {

// A model or runtime failure: a model file that will not load, a prompt that does not fit, a failed decode.
class InferenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Role { System, User, Assistant };

// Role spelling used by chat templates: "system", "user", "assistant".
std::string RoleName(Role role);

struct Message {
    Role role = Role::User;
    std::string content = {};
};

// A chat prompt. messages[0, stableCount) is the stable prefix (layers L0-L3); the rest is the volatile tail.
// The llama.cpp backend reuses the KV cache by longest common token prefix, so stableCount is informational for
// it; a cloud backend places its prompt-cache breakpoint there.
struct Prompt {
    std::vector<Message> messages = {};
    std::size_t stableCount = 0;
};

struct Sampling {
    float temperature = 0.0f; // <= 0 means greedy decoding.
    int topK = 40;
    float topP = 0.95f;
    std::uint32_t seed = 0;
};

struct GenerationSpec {
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained.
    int maxTokens = 256;
    Sampling sampling = {};
};

struct Completion {
    std::string text = {};
    int promptTokens = 0;     // Tokens in the templated prompt.
    int tokensReused = 0;     // Prompt tokens served from the KV cache.
    int tokensEvaluated = 0;  // Prompt tokens computed for this call.
    int tokensGenerated = 0;
    bool stoppedAtLimit = false; // Generation reached maxTokens before an end-of-generation token.
    bool cancelled = false;
    double promptMs = 0.0;
    double generateMs = 0.0;
};

enum class EmbedPurpose { Query, Document };

// Receives generated text as it streams. Pieces never split a UTF-8 character.
using TokenCallback = std::function<void(std::string_view piece)>;

// The engine's only view of a language model (architecture spec, "Inference interface").
class LlmBackend {
public:
    virtual ~LlmBackend() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;

    // Generates the assistant's reply to `prompt`. Throws InferenceError if prompt + spec.maxTokens exceeds the
    // context. Cancellation through `stop` returns early with completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

} // namespace morpheus::inference
```

- [ ] **Step 4: Create `src/inference/Llm.cpp` and `src/inference/CMakeLists.txt`**

`src/inference/Llm.cpp`:

```cpp
#include "inference/Llm.h"

#include "core/Errors.h"

namespace morpheus::inference {

std::string RoleName(Role role)
{
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    }
    throw core::ContractError("RoleName: unhandled Role");
}

} // namespace morpheus::inference
```

`src/inference/CMakeLists.txt`:

```cmake
add_library(morpheus_inference STATIC
    Llm.cpp
)
target_include_directories(morpheus_inference PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
morpheus_target_defaults(morpheus_inference)
```

- [ ] **Step 5: Create `tests/fakes/FakeBackend.h`**

```cpp
#pragma once

#include "core/Errors.h"
#include "inference/Llm.h"

namespace morpheus::test {

// Deterministic stand-in for context tests. One token per 4 bytes (rounded up), plus 4 template tokens per
// message. Generation, embeddings and state are not used by those tests and throw if called.
class FakeBackend final : public inference::LlmBackend {
private:
    int _contextSize = 0;

public:
    explicit FakeBackend(int contextSize)
        : _contextSize(contextSize)
    {
    }

    int CountTokens(std::string_view text) override
    {
        return static_cast<int>((text.size() + 3) / 4);
    }

    int CountPromptTokens(const inference::Prompt& prompt) override
    {
        int total = 0;
        for (const inference::Message& message : prompt.messages) {
            total += CountTokens(message.content) + 4;
        }
        return total;
    }

    int ContextSize() const override
    {
        return _contextSize;
    }

    inference::Completion Complete(const inference::Prompt&, const inference::GenerationSpec&,
                                   const inference::TokenCallback&, std::stop_token) override
    {
        throw core::ContractError("FakeBackend::Complete is not available");
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>&, inference::EmbedPurpose) override
    {
        throw core::ContractError("FakeBackend::Embed is not available");
    }

    std::string EmbeddingModelId() const override
    {
        throw core::ContractError("FakeBackend::EmbeddingModelId is not available");
    }

    std::vector<std::uint8_t> SaveState() override
    {
        throw core::ContractError("FakeBackend::SaveState is not available");
    }

    void RestoreState(std::span<const std::uint8_t>) override
    {
        throw core::ContractError("FakeBackend::RestoreState is not available");
    }
};

} // namespace morpheus::test
```

- [ ] **Step 6: Restructure the CMake build**

In the root `CMakeLists.txt`:

1. Add, after the existing options:

```cmake
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend (fetches and builds llama.cpp)" ON)
option(MORPHEUS_VULKAN "Build llama.cpp with the Vulkan GPU backend (needs the Vulkan SDK)" ON)

if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)
    message(FATAL_ERROR "MORPHEUS_BUILD_APP needs MORPHEUS_BUILD_LLAMA.")
endif()
```

2. Move the `llama` FetchContent block out of `if(MORPHEUS_BUILD_APP)` into its own block, placed before it:

```cmake
if(MORPHEUS_BUILD_LLAMA)
    # GPU backend for llama.cpp. FORCE so that toggling MORPHEUS_VULKAN reconfigures the fetched llama.cpp.
    set(GGML_VULKAN ${MORPHEUS_VULKAN} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: redirect to the new repo: ggml-org/llama.cpp , probably update the targeted commit?
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Its headers are external: no /W4 /WX
    )
    FetchContent_MakeAvailable(llama)
endif()
```

   SDL3 and imgui stay inside `if(MORPHEUS_BUILD_APP)`.

3. Add `add_subdirectory(src/inference)` right after `add_subdirectory(src/store)`.

In `tests/CMakeLists.txt`, right after the `file(GLOB_RECURSE ...)` line, add:

```cmake
# Model-backed tests need llama.cpp; presets without it skip their sources entirely.
if(NOT MORPHEUS_BUILD_LLAMA)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/llama/")
endif()
```

and add `morpheus_inference` to `morpheus_tests`'s `target_link_libraries` list.

In `CMakePresets.json`:
- Add `"MORPHEUS_BUILD_LLAMA": "ON"` and `"MORPHEUS_VULKAN": "ON"` to the `dev` preset's `cacheVariables`.
- Add `"MORPHEUS_BUILD_LLAMA": "OFF"` to the `tests` preset's `cacheVariables`. The `asan` preset inherits it.

- [ ] **Step 7: Build and run under `tests`. Expect a pass.**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: configuration does not fetch llama.cpp, and all tests pass, including `[inference]`.

- [ ] **Step 8: Configure and build `dev` with Vulkan. Expect success.**

Run (long; use a 600000 ms timeout or a background run): `cmake --preset dev && cmake --build --preset dev`

Expected:
- llama.cpp configures with `GGML_VULKAN=ON`: the configure output mentions Vulkan and finds `glslc`.
- `build/dev/bin/Debug` contains `ggml-vulkan.dll` and `morpheus.exe`.

If Vulkan fails to configure, report the exact error. Do not switch to CPU on your own.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt CMakePresets.json tests src/inference
git commit -m "Add the inference interface and build llama.cpp with Vulkan" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

