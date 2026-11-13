### Task 9: The model-backed grammar tests

The same 1000 values (same seed, same draws) are checked by llama.cpp's grammar sampler through `LlamaCppBackend::CheckGrammar`; each `FromSchema` grammar for the test schemas parses; hand-made invalid actions are rejected; the rebuilt review grammars parse; and llama.cpp agrees with the test matcher on `IntRange` grammars. These tests exercise code that already exists, so they pass when first run; a failure is a real disagreement between the library and llama.cpp and must be investigated (superpowers:systematic-debugging), not papered over.

**Files:**
- Modify: `tests/llama/LlamaGrammarTests.cpp`

**Interfaces:**
- Consumes: `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`); `LlamaCppBackend::CheckGrammar(grammar, text)` ("" when the text fits; a reason otherwise; `ContractError` for a grammar that does not parse); `grammar::FromSchema`, `grammar::GrammarBuilder`, `grammar::IntRange`; `turn::BuildReviewGrammar`; `test::RandomValue`, `test::PROPERTY_SEED`, `test::PROPERTY_DRAWS`, `test::CatalogSchema`, `test::EveryKeywordSchema`, `test::GbnfMatcher`.
- Produces: four `[llama]` test cases whose names start with `llama.cpp`.

- [ ] **Step 1: Write the tests**

In `tests/llama/LlamaGrammarTests.cpp`, replace

```cpp
#include <algorithm>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"
```

with

```cpp
#include <algorithm>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
#include "core/World.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/RandomValue.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"
```

and append:

```cpp
TEST_CASE("llama.cpp accepts the random values of each test schema against the schema's grammar", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<nlohmann::json> schemas = {test::CatalogSchema(), test::EveryKeywordSchema()};
    for (const nlohmann::json& schema : schemas) {
        const std::string gbnf = grammar::FromSchema(schema);
        // The grammar parses (one that does not is a ContractError) and wants a whole value.
        CHECK(backend.CheckGrammar(gbnf, "") == "the text ends before the grammar does");
        // The same values as the fast suite's property test: the same seed, the same draws, written compact.
        std::mt19937_64 random(test::PROPERTY_SEED);
        for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
            const std::string text = test::RandomValue(schema, random).dump();
            INFO("value " << i << ": " << text);
            CHECK(backend.CheckGrammar(gbnf, text) == "");
        }
    }
}

TEST_CASE("llama.cpp rejects invalid actions against the catalog grammar", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::string gbnf = grammar::FromSchema(test::CatalogSchema());
    const std::vector<std::string> invalid = {
        R"({"action":"dance","params":{}})",                                             // an unknown action
        R"({"action":"attack","params":{"target":"0001C3D2"}})",                         // an actor who is not present
        R"({"action":"attack","params":{}})",                                            // a missing argument
        R"({"action":"wait","params":{},"intent":"rest"})",                              // an extra key
        R"({"action":"give_item","params":{"item":")" + std::string(49, 'a') + R"("}})", // an item over 48 characters
        R"({"action": "wait","params":{}})",                                             // whitespace
    };
    for (const std::string& text : invalid) {
        INFO(text);
        CHECK(backend.CheckGrammar(gbnf, text) != "");
    }
}

TEST_CASE("llama.cpp parses the review grammars built on the builder", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // A grammar llama.cpp cannot parse is a ContractError, so each check also proves the grammar compiles.
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true),
                               R"({"consistent":false,"problems":["The roll failed."],"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, false), R"({"consistent":true,"problems":[]})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(false, true), R"({"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true), R"({"consistent":true,"problems":["x"],"digest":""})") != "");
}

TEST_CASE("llama.cpp and the test matcher agree on IntRange grammars", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<std::pair<long long, long long>> ranges = {{-15, 123}, {0, 255}, {-1000, -91}, {37, 120}};
    for (const auto& [low, high] : ranges) {
        const std::string gbnf = grammar::GrammarBuilder().Text(grammar::IntRange(low, high));
        const test::GbnfMatcher matcher(gbnf);
        // Other spellings, then a window around each end.
        std::vector<std::pair<std::string, bool>> probes = {{"-0", false}, {"00", false}, {"007", false}, {"-01", false},
                                                            {"+5", false}};
        for (long long value = low - 12; value <= low + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (long long value = high - 12; value <= high + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (const auto& [text, inRange] : probes) {
            INFO("IntRange(" << low << ", " << high << ") on '" << text << "'");
            CHECK(backend.CheckGrammar(gbnf, text).empty() == inRange);
            CHECK(matcher.Accepts(text) == inRange);
        }
    }
}
```

- [ ] **Step 2: Build the dev tree**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`
Expected: builds with no warnings.

- [ ] **Step 3: Run the model-backed grammar tests**

Run (Git Bash, from the repo root; the variables are the README's):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"
```

Expected: `All tests passed` (the existing "llama.cpp accepts the generated grammars and they constrain real output", which now builds the resolve and lookup grammars with the builder, the existing `CheckGrammar` test, the existing "llama.cpp log lines reach the log sink", and these four). If any test reports SKIPPED, a variable is unset: set it and run again; a skip does not count as done.

- [ ] **Step 4: Commit**

```bash
git add tests/llama/LlamaGrammarTests.cpp
git commit -m "Check schema grammars, the review grammars and IntRange with llama.cpp" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

