### Task 6: Embeddings and KV-cache save/restore tests

**Files:**
- Create: `tests/llama/LlamaEmbedStateTests.cpp`

**Interfaces:**
- Consumes: `LlamaCppBackend::Embed`, `EmbeddingModelId`, `SaveState`, `RestoreState` and `SharedBackend()` (Task 5).
- Produces: model-backed tests only. If they expose a bug, fix it in `src/llama/LlamaCppBackend.cpp`.

- [ ] **Step 1: Write the tests**

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <string>
#include <vector>

#include "llama/LlamaTestModels.h"

using namespace morpheus::inference;
using morpheus::test::SharedBackend;

namespace {

float Dot(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

Prompt SimplePrompt(const std::string& question)
{
    Prompt prompt;
    prompt.messages = {{Role::System, "You are a terse assistant."}, {Role::User, question}};
    prompt.stableCount = 1;
    return prompt;
}

} // namespace

TEST_CASE("Embeddings are normalized and rank related texts higher", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"a tracked crawler crossing the desert", "a vehicle driving over sand dunes", "a recipe for tomato soup"},
        EmbedPurpose::Document);

    REQUIRE(vectors.size() == 3);
    REQUIRE_FALSE(vectors[0].empty());
    for (const std::vector<float>& vector : vectors) {
        CHECK(vector.size() == vectors[0].size());
        CHECK(Dot(vector, vector) == Catch::Approx(1.0f).margin(1e-4));
    }
    CHECK(Dot(vectors[0], vectors[1]) > Dot(vectors[0], vectors[2]));
    CHECK_FALSE(backend.EmbeddingModelId().empty());
}

TEST_CASE("Query and document prefixes give different embeddings", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    const std::vector<float> query = backend.Embed({"water supplies"}, EmbedPurpose::Query).at(0);
    const std::vector<float> document = backend.Embed({"water supplies"}, EmbedPurpose::Document).at(0);
    CHECK(Dot(query, document) < 0.9999f);
}

TEST_CASE("A saved KV cache restores prefix reuse", "[llama]")
{
    LlmBackend& backend = SharedBackend();
    GenerationSpec spec;
    spec.maxTokens = 4;

    const Prompt original = SimplePrompt("Name a desert animal.");
    backend.Complete(original, spec, {}, {});
    const std::vector<std::uint8_t> saved = backend.SaveState();

    // A different prompt overwrites the cache...
    backend.Complete(SimplePrompt("Count to three."), spec, {}, {});

    // ...and restoring brings the original prefix back.
    backend.RestoreState(saved);
    const Completion again = backend.Complete(original, spec, {}, {});
    CHECK(again.tokensEvaluated == 1);
    CHECK(again.tokensReused == again.promptTokens - 1);
}
```

- [ ] **Step 2: Build and run**

Run: `cmake --build --preset dev && ./build/dev/bin/Debug/morpheus_tests.exe "[llama]"`

Expected:
- **With the models:** all `[llama]` tests pass.
- **Without them:** all are skipped.
- **On a failure:** fix the backend, and record the root cause in the report.

- [ ] **Step 3: Commit**

```bash
git add tests/llama src/llama
git commit -m "Test embeddings and KV-cache save/restore" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

