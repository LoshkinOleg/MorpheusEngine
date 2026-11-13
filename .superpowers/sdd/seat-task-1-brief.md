### Task 1: The grammar check on the backend interface

`LlmBackend` gains `CheckGrammar`, and every implementation gets it in the same task (a pure virtual breaks any that lacks it). llama.cpp's grammar sampler checks the text's tokens one by one, with no decode, so the KV cache is untouched.

**Files:**
- Modify: `src/inference/Llm.h`, `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp`, `src/turn/Replay.h`, `src/turn/Replay.cpp`, `tests/fakes/FakeBackend.h`, `tests/fakes/ScriptedBackend.h`
- Test: `tests/turn/ReplayTests.cpp`, `tests/llama/LlamaGrammarTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `virtual std::string inference::LlmBackend::CheckGrammar(const std::string& grammar, std::string_view text) = 0;` ("" when the text fits; a reason otherwise; `ContractError` for an empty or unparsable grammar).
  - `test::ScriptedBackend::GrammarCheck { std::string grammar; std::string text; }`, `void SetGrammarVerdict(std::string verdict)`, `const std::vector<GrammarCheck>& GrammarChecks() const`; its `CheckGrammar` records the check and returns the verdict ("" by default).
  - `test::FakeBackend::CheckGrammar` throws `ContractError("FakeBackend::CheckGrammar is not available")`.

- [ ] **Step 1: Write the failing forwarding test**

Append to `tests/turn/ReplayTests.cpp`:

```cpp
TEST_CASE("RecordingBackend forwards the grammar check and records nothing for it", "[replay]")
{
    test::ScriptedBackend inner(16384);
    turn::RecordingBackend recording(inner);
    const std::string grammar = R"(root ::= "yes")";

    // ScriptedBackend's default verdict: every text fits.
    CHECK(recording.CheckGrammar(grammar, "yes").empty());
    inner.SetGrammarVerdict("the grammar rejects 'no' at the start");
    CHECK(recording.CheckGrammar(grammar, "no") == "the grammar rejects 'no' at the start");

    REQUIRE(inner.GrammarChecks().size() == 2);
    CHECK(inner.GrammarChecks()[1].grammar == grammar);
    CHECK(inner.GrammarChecks()[1].text == "no");
    CHECK(recording.Replies().empty()); // A check is not a completion.
}
```

- [ ] **Step 2: Write the model-backed test**

In `tests/llama/LlamaGrammarTests.cpp`, replace

```cpp
#include <catch2/catch_test_macros.hpp>
```

with

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
```

replace

```cpp
#include "context/WorldReference.h"
#include "core/Changes.h"
```

with

```cpp
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
```

replace

```cpp
using namespace morpheus;
```

with

```cpp
using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
```

and append:

```cpp
TEST_CASE("LlamaCppBackend::CheckGrammar tells whether a text fits a grammar and where it stops fitting", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::string grammar = R"(root ::= "{\"answer\":\"" ("yes" | "no") "\"}")";

    // Texts the grammar accepts from start to end.
    CHECK(backend.CheckGrammar(grammar, R"({"answer":"yes"})").empty());
    CHECK(backend.CheckGrammar(grammar, R"({"answer":"no"})").empty());

    // A text that breaks the grammar: the reason names the rejected token and the text before it.
    const std::string broken = backend.CheckGrammar(grammar, R"({"answer":"maybe"})");
    CHECK_THAT(broken, ContainsSubstring("the grammar rejects") && ContainsSubstring("answer"));
    const std::string trailing = backend.CheckGrammar(grammar, R"({"answer":"yes"} and more)");
    CHECK_THAT(trailing, ContainsSubstring("the grammar rejects") && ContainsSubstring("yes"));

    // A text that ends early.
    CHECK(backend.CheckGrammar(grammar, R"({"answer":"ye)") == "the text ends before the grammar does");
    CHECK(backend.CheckGrammar(grammar, "") == "the text ends before the grammar does");

    // A grammar that does not parse, or none at all, is an engine bug.
    CHECK_THROWS_AS(backend.CheckGrammar("root ::= (", "x"), core::ContractError);
    CHECK_THROWS_AS(backend.CheckGrammar("", "x"), core::ContractError);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/turn/ReplayTests.cpp`: `'CheckGrammar': is not a member of 'morpheus::turn::RecordingBackend'` and `'SetGrammarVerdict': is not a member of 'morpheus::test::ScriptedBackend'` (the llama test is not compiled by this preset).

- [ ] **Step 4: Add the method to the interface**

In `src/inference/Llm.h`, replace

```cpp
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;
```

with

```cpp
    virtual Completion Complete(const Prompt& prompt, const GenerationSpec& spec, const TokenCallback& onToken,
                                std::stop_token stop) = 0;

    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end.
    // Returns "" when it is; otherwise a short reason naming where it stops fitting (the first rejected token and the
    // text before it, or "the text ends before the grammar does"). Throws ContractError for a grammar that does not
    // parse. An empty grammar is a ContractError too: there is nothing to check against.
    virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;
```

- [ ] **Step 5: The test fakes**

In `tests/fakes/FakeBackend.h`, replace

```cpp
// Deterministic stand-in for context tests. One token per 4 bytes (rounded up), plus 4 template tokens per
// message. Generation, embeddings and state are not used by those tests and throw if called.
```

with

```cpp
// Deterministic stand-in for context tests. One token per 4 bytes (rounded up), plus 4 template tokens per
// message. Generation, the grammar check, embeddings and state are not used by those tests and throw if called.
```

and replace

```cpp
        throw core::ContractError("FakeBackend::Complete is not available");
    }
```

with

```cpp
        throw core::ContractError("FakeBackend::Complete is not available");
    }

    std::string CheckGrammar(const std::string&, std::string_view) override
    {
        throw core::ContractError("FakeBackend::CheckGrammar is not available");
    }
```

In `tests/fakes/ScriptedBackend.h`, replace the last line of the class comment

```cpp
// EMBED_DIMS dimensions, L2-normalized), so texts sharing words score higher. Save/RestoreState keep nothing.
```

with

```cpp
// EMBED_DIMS dimensions, L2-normalized), so texts sharing words score higher. Save/RestoreState keep nothing.
// CheckGrammar records each check and answers the verdict SetGrammarVerdict set: "" (the text fits) by default.
```

replace

```cpp
    struct Call {
        inference::Prompt prompt = {};
        inference::GenerationSpec spec = {};
    };
```

with

```cpp
    struct Call {
        inference::Prompt prompt = {};
        inference::GenerationSpec spec = {};
    };

    // One CheckGrammar call's arguments.
    struct GrammarCheck {
        std::string grammar = {};
        std::string text = {};
    };
```

replace

```cpp
    std::chrono::milliseconds _pieceDelay = std::chrono::milliseconds(0);
```

with

```cpp
    std::chrono::milliseconds _pieceDelay = std::chrono::milliseconds(0);
    std::string _grammarVerdict = {}; // What CheckGrammar answers.
    std::vector<GrammarCheck> _grammarChecks = {};
```

replace

```cpp
    // Sleeps before each streamed piece, so tests can cancel mid-reply.
    void SetPieceDelay(std::chrono::milliseconds delay)
    {
        _pieceDelay = delay;
    }
```

with

```cpp
    // Sleeps before each streamed piece, so tests can cancel mid-reply.
    void SetPieceDelay(std::chrono::milliseconds delay)
    {
        _pieceDelay = delay;
    }

    // What CheckGrammar answers from now on: "" when every text fits, else the reason a real backend would give.
    void SetGrammarVerdict(std::string verdict)
    {
        _grammarVerdict = std::move(verdict);
    }

    // Every CheckGrammar call so far, in order.
    const std::vector<GrammarCheck>& GrammarChecks() const
    {
        return _grammarChecks;
    }
```

and insert, right before `std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose) override`:

```cpp
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override
    {
        MORPHEUS_REQUIRE(!grammar.empty(), "CheckGrammar: no grammar to check against");
        _grammarChecks.push_back({grammar, std::string(text)});
        return _grammarVerdict;
    }

```

- [ ] **Step 6: RecordingBackend forwards it**

In `src/turn/Replay.h`, in `RecordingBackend`, replace

```cpp
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
```

with

```cpp
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
```

In `src/turn/Replay.cpp`, replace

```cpp
        _replies[kind].push_back(completion.text);
    }
    return completion;
}
```

with

```cpp
        _replies[kind].push_back(completion.text);
    }
    return completion;
}

std::string RecordingBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text); // A check is not a completion: nothing is recorded.
}
```

- [ ] **Step 7: Run the tests-preset tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`
Expected: all `[replay]` tests pass.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 286` (285 before this plan, plus this task's one).

- [ ] **Step 8: LlamaCppBackend's grammar check**

In `src/llama/LlamaCppBackend.h`, replace

```cpp
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
```

with

```cpp
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    // The grammar sampler alone over the text's tokens: no decode, so the KV cache is untouched.
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
```

In `src/llama/LlamaCppBackend.cpp`, replace

```cpp
// Buffer for one token piece (llama_token_to_piece); a longer piece is an error.
constexpr std::size_t TOKEN_PIECE_BUFFER_BYTES = 256;
```

with

```cpp
// Buffer for one token piece (llama_token_to_piece); a longer piece is an error.
constexpr std::size_t TOKEN_PIECE_BUFFER_BYTES = 256;

// How much of the text before a rejected token CheckGrammar's reason quotes, in bytes.
constexpr std::size_t GRAMMAR_CONTEXT_BYTES = 60;
```

and insert, right before `std::vector<std::vector<float>> LlamaCppBackend::Embed(`:

```cpp
std::string LlamaCppBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    MORPHEUS_REQUIRE(!grammar.empty(), "CheckGrammar: no grammar to check against");
    const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());

    // The grammar sampler alone, built as Complete builds it. Nothing is decoded, so the KV cache is untouched.
    std::unique_ptr<llama_sampler, SamplerDeleter> pGrammar(llama_sampler_init_grammar(pVocab, grammar.c_str(), "root"));
    MORPHEUS_REQUIRE(pGrammar != nullptr, "CheckGrammar: the GBNF grammar does not parse");

    // Whether the grammar, in its current state, allows `token` next: applied to a candidate array holding only that
    // token, it masks a rejected token's logit to -infinity (as Complete checks a sampled token).
    auto allows = [&pGrammar](llama_token token) {
        llama_token_data single = {token, 1.0f, 0.0f};
        llama_token_data_array singleArray = {&single, 1, -1, false};
        llama_sampler_apply(pGrammar.get(), &singleArray);
        return single.logit != -std::numeric_limits<float>::infinity();
    };

    // The text tokenized as the model would have written it: plain text, no special tokens. Each token must be
    // allowed where it stands, and is then accepted, advancing the grammar past it. Only allowed tokens are
    // accepted, so the grammar always has a way to go on.
    std::string accepted; // The text of the tokens accepted so far.
    for (const std::int32_t token : Tokenize(pVocab, text, false, false)) {
        char piece[TOKEN_PIECE_BUFFER_BYTES];
        const int32_t length = llama_token_to_piece(pVocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, false);
        MORPHEUS_REQUIRE(length >= 0,
                         std::format("CheckGrammar: a token piece is longer than {} bytes", TOKEN_PIECE_BUFFER_BYTES));
        const std::string_view pieceText(piece, static_cast<std::size_t>(length));
        if (!allows(token)) {
            // The first rejected token and the text before it: its last GRAMMAR_CONTEXT_BYTES bytes, starting on a
            // whole UTF-8 character.
            if (accepted.empty()) {
                return std::format("the grammar rejects '{}' at the start", pieceText);
            }
            std::size_t start = accepted.size() > GRAMMAR_CONTEXT_BYTES ? accepted.size() - GRAMMAR_CONTEXT_BYTES : 0;
            while (start < accepted.size() && (static_cast<unsigned char>(accepted[start]) & 0xC0) == 0x80) {
                ++start; // A continuation byte: start at the next character.
            }
            return std::format("the grammar rejects '{}' after '{}{}'", pieceText, start > 0 ? "..." : "",
                               std::string_view(accepted).substr(start));
        }
        llama_sampler_accept(pGrammar.get(), token);
        accepted += pieceText;
    }

    // Where the text ends, the grammar must be complete: only then does it allow the end of generation.
    const llama_token endToken =
        llama_vocab_eot(pVocab) != LLAMA_TOKEN_NULL ? llama_vocab_eot(pVocab) : llama_vocab_eos(pVocab);
    if (!allows(endToken)) {
        return "the text ends before the grammar does";
    }
    return {};
}

```

- [ ] **Step 9: Build the dev targets and run the model-backed test**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests morpheus morpheus_play morpheus_probe`
Expected: builds with no warnings (every `LlmBackend` in the dev tree now implements `CheckGrammar`).

Run (Git Bash, from the repo root; the variables are the README's):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "LlamaCppBackend::CheckGrammar*"
```

Expected: the test case passes (`All tests passed`). If either variable is unset it reports SKIPPED: set them and run again; a skip does not count as done.

- [ ] **Step 10: Commit**

```bash
git add src/inference/Llm.h src/llama/LlamaCppBackend.h src/llama/LlamaCppBackend.cpp src/turn/Replay.h src/turn/Replay.cpp tests/fakes/FakeBackend.h tests/fakes/ScriptedBackend.h tests/turn/ReplayTests.cpp tests/llama/LlamaGrammarTests.cpp
git commit -m "Add LlmBackend::CheckGrammar" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

