### Task 1: Split the interface, add the slot, move SamplingFrom to turn

`LlmBackend` becomes the union of three interfaces. `CheckGrammar` stays on `LlmBackend` for now (Task 3 removes it), so no implementation changes in this task. `SamplingFrom` moves to `turn`, which takes `inference`'s last use of `core::Settings` away.

**Files:**
- Modify: `src/inference/Llm.h`, `src/inference/Llm.cpp`, `src/inference/CMakeLists.txt`
- Modify: `src/llama/CMakeLists.txt`
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Modify: `tools/probe/main.cpp`, `tools/probe/CMakeLists.txt`
- Modify: `tests/inference/LlmTests.cpp`
- Delete: `tests/inference/SamplingTests.cpp`; Create: `tests/turn/SamplingTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `inference::Tokenizer`, `inference::ChatModel`, `inference::Embedder` (pure virtual, as below); `class inference::LlmBackend : public Tokenizer, public ChatModel, public Embedder` (holding only `CheckGrammar` until Task 3); `int inference::GenerationSpec::slot = -1`; `inference::Sampling turn::SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)` in `turn/Session.h`. `inference/Llm.h` no longer includes `core/Settings.h`.

- [ ] **Step 1: Write the failing tests**

In `tests/inference/LlmTests.cpp`, add `#include <type_traits>` after `#include <catch2/catch_test_macros.hpp>` (with a blank line between, as the other files group standard headers), and append:

```cpp
TEST_CASE("LlmBackend is the union of Tokenizer, ChatModel and Embedder", "[inference]")
{
    static_assert(std::is_base_of_v<Tokenizer, LlmBackend>);
    static_assert(std::is_base_of_v<ChatModel, LlmBackend>);
    static_assert(std::is_base_of_v<Embedder, LlmBackend>);

    // A caller can hold one part alone, as sub-project D's code will.
    morpheus::test::FakeBackend fake(1000);
    Tokenizer& tokenizer = fake;
    CHECK(tokenizer.CountTokens("abcde") == 2);
    CHECK(tokenizer.ContextSize() == 1000);
    ChatModel& chat = fake;
    CHECK_THROWS_AS(chat.SaveState(), morpheus::core::ContractError);
    Embedder& embedder = fake;
    CHECK_THROWS_AS(embedder.EmbeddingModelId(), morpheus::core::ContractError);
}

TEST_CASE("GenerationSpec's slot defaults to the backend's default slot", "[inference]")
{
    CHECK(GenerationSpec{}.slot == -1);
}
```

Delete the old sampling test and create its replacement in `turn`:

```bash
git rm -q tests/inference/SamplingTests.cpp
```

Create `tests/turn/SamplingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "core/Settings.h"
#include "turn/Session.h"

using namespace morpheus;

TEST_CASE("SamplingFrom converts settings and carries the seed", "[turn]")
{
    const core::SamplingSettings settings = {0.25, 30, 0.5};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 77);
    CHECK(sampling.temperature == 0.25f);
    CHECK(sampling.topK == 30);
    CHECK(sampling.topP == 0.5f);
    CHECK(sampling.seed == 77u);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `'Tokenizer': undeclared identifier` in `tests/inference/LlmTests.cpp`, `'slot': is not a member of 'morpheus::inference::GenerationSpec'`, and `'SamplingFrom': is not a member of 'morpheus::turn'` in `tests/turn/SamplingTests.cpp`.

- [ ] **Step 3: Split the interface**

Replace the whole of `src/inference/Llm.h` with:

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
    std::string grammar = {}; // GBNF with a "root" rule; empty means unconstrained. Applies to the answer only.
    int maxTokens = 256;      // The answer: the text after any thinking.
    // > 0 lets a thinking model reason before it answers, in a block of at most this many tokens (its markers
    // included). The block is unconstrained, never streamed and kept out of Completion::text. 0: no thinking.
    int thinkingMaxTokens = 0;
    Sampling sampling = {};
    // The server slot whose prompt cache the call uses; -1 is the backend's default slot. The in-process backend has
    // one cache and ignores it; LlamaServerBackend sends it as id_slot. Morpheus's own game leaves it at -1.
    int slot = -1;
};

struct Completion {
    std::string text = {};     // The answer. Empty when the model ended its reply inside the thinking block.
    std::string thinking = {}; // The reasoning written in the thinking block, without its markers.
    int thinkingTokens = 0;    // Tokens the thinking block took, markers included.
    bool thinkingStoppedAtLimit = false; // The block reached thinkingMaxTokens and the backend closed it.
    double thinkingMs = 0.0;
    int promptTokens = 0;     // Tokens in the templated prompt.
    int tokensReused = 0;     // Prompt tokens served from the KV cache.
    int tokensEvaluated = 0;  // Prompt tokens computed for this call.
    int tokensGenerated = 0;     // Answer tokens.
    bool stoppedAtLimit = false; // The answer reached maxTokens before an end-of-generation token.
    bool cancelled = false;
    double promptMs = 0.0;
    double generateMs = 0.0; // The answer; thinkingMs covers the thinking block.
};

enum class EmbedPurpose { Query, Document };

// Receives generated text as it streams. Pieces never split a UTF-8 character.
using TokenCallback = std::function<void(std::string_view piece)>;

// Counts tokens as the chat model reads them (architecture spec, "Inference interface").
class Tokenizer {
public:
    virtual ~Tokenizer() = default;

    // Tokens in raw text: no chat template, no special tokens.
    virtual int CountTokens(std::string_view text) = 0;

    // Tokens in the full templated prompt, exactly as Complete would evaluate it without thinking. A call with
    // thinking evaluates no more than that: the markers of its thinking block count against thinkingMaxTokens.
    virtual int CountPromptTokens(const Prompt& prompt) = 0;

    virtual int ContextSize() const = 0;
};

// Generates the model's replies, and holds the chat cache they reuse.
class ChatModel {
public:
    virtual ~ChatModel() = default;

    // Generates the assistant's reply to `prompt`: with spec.thinkingMaxTokens > 0 a thinking block first, then the
    // answer, which alone is streamed to onToken and constrained by spec.grammar. Throws InferenceError if prompt +
    // spec.thinkingMaxTokens + spec.maxTokens exceeds the context, or if thinking is asked of a model without a
    // thinking mode. Throws ContractError for maxTokens <= 0, a negative thinkingMaxTokens, one too small to close
    // the block, or a grammar that does not parse. Cancellation through `stop` returns early with
    // completion.cancelled set; it is not an error.
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // The chat KV cache and the tokens it holds, so the GPU can be handed to another model and the cache restored.
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(std::span<const std::uint8_t> state) = 0;
};

// Turns texts into vectors for retrieval.
class Embedder {
public:
    virtual ~Embedder() = default;

    // One L2-normalized vector per text, in order.
    virtual std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, EmbedPurpose purpose) = 0;

    // Identifies the embedding model, for store::EmbeddingRow::model.
    virtual std::string EmbeddingModelId() const = 0;
};

// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {
public:
    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end.
    // Returns "" when it is; otherwise a short reason naming where it stops fitting (the first rejected token and the
    // text before it, or "the text ends before the grammar does"). Throws ContractError for a grammar that does not
    // parse. An empty grammar is a ContractError too: there is nothing to check against.
    virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;
};

} // namespace morpheus::inference
```

In `src/inference/Llm.cpp`, delete the `SamplingFrom` definition (the whole function, from `Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)` to its closing brace, and the blank line before it). `RoleName` and the `#include "core/Errors.h"` stay.

In `src/inference/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
```

with

```cmake
# No library dependencies: Llm.cpp uses only core/Errors.h (ContractError), which is header-only.
```

In `src/llama/CMakeLists.txt`, `morpheus_llama` used `core` (`core::PathText`) through `inference`; it links it itself now. Replace

```cmake
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
```

with

```cmake
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE morpheus_core llama
)
```

- [ ] **Step 4: Move SamplingFrom to turn**

In `src/turn/Session.h`, after the line `constexpr int REVIEW_RETRY_OFFSET = 1 << 23;`, insert:

```cpp

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);
```

In `src/turn/Session.cpp`, after the line `} // namespace` that closes the anonymous namespace (just before `Session::Session(`), insert:

```cpp
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)
{
    inference::Sampling sampling;
    sampling.temperature = static_cast<float>(settings.temperature);
    sampling.topK = settings.topK;
    sampling.topP = static_cast<float>(settings.topP);
    sampling.seed = seed;
    return sampling;
}

```

Then replace each of the six `inference::SamplingFrom(` in `src/turn/Session.cpp` with `SamplingFrom(` (the file is inside `namespace morpheus::turn`). Check: `grep -n "inference::SamplingFrom" src tools tests -r` prints only `tools/probe/main.cpp`.

In `tools/probe/main.cpp`, add `#include "turn/Session.h"` after `#include "llama/LlamaCppBackend.h"`, and replace

```cpp
        spec.sampling = inference::SamplingFrom(settings.narrateSampling, 1234);
```

with

```cpp
        spec.sampling = turn::SamplingFrom(settings.narrateSampling, 1234);
```

In `tools/probe/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_llama)
```

with

```cmake
target_link_libraries(morpheus_probe PRIVATE morpheus_context morpheus_turn morpheus_llama)
```

If the build reports `'Settings': is not a member of 'morpheus::core'` (or `SamplingSettings`, `BudgetSettings`) in a file that relied on `inference/Llm.h` including `core/Settings.h`, add `#include "core/Settings.h"` to that file. (A search found every such file already includes it through another header.)

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 370`.

- [ ] **Step 6: Build the dev targets**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings (the probe links `morpheus_turn`; `morpheus_llama` links `morpheus_core` itself).

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/inference/Llm.h src/inference/Llm.cpp src/inference/CMakeLists.txt src/llama/CMakeLists.txt src/turn/Session.h src/turn/Session.cpp tools/probe/main.cpp tools/probe/CMakeLists.txt tests/inference/LlmTests.cpp tests/turn/SamplingTests.cpp
git add src/inference/Llm.h src/inference/Llm.cpp src/inference/CMakeLists.txt src/llama/CMakeLists.txt src/turn/Session.h src/turn/Session.cpp tools/probe/main.cpp tools/probe/CMakeLists.txt tests/inference/LlmTests.cpp tests/inference/SamplingTests.cpp tests/turn/SamplingTests.cpp
git commit -m "Split LlmBackend into Tokenizer, ChatModel and Embedder; add GenerationSpec::slot; move SamplingFrom to turn" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Expected: `check_bytes` prints nothing.

---

