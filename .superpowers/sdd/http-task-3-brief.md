### Task 3: CheckGrammar leaves the interface; the model seat checks with grammar::Check

`LlmBackend` becomes exactly the union of its three parts. The model seat's Check calls `grammar::Check`; `LlamaCppBackend` keeps its llama.cpp-based checker as a plain method for the `[llama]` agreement tests; every other implementation drops `CheckGrammar`, and `ScriptedBackend` loses its grammar-verdict hooks.

**Files:**
- Modify: `src/inference/Llm.h`
- Modify: `src/llama/LlamaCppBackend.h`
- Modify: `src/turn/Replay.h`, `src/turn/Replay.cpp`
- Modify: `src/app/model/ModelSeat.h`, `src/app/model/ModelSeat.cpp`, `src/app/model/ModelSeatBackend.h`, `src/app/model/ModelSeatBackend.cpp`, `src/app/CMakeLists.txt`
- Modify: `tests/fakes/FakeBackend.h`, `tests/fakes/ScriptedBackend.h`
- Test: `tests/app/ModelSeatTests.cpp`, `tests/app/ModelSeatBackendTests.cpp`, `tests/app/UiTests.cpp`, `tests/turn/ReplayTests.cpp`

**Interfaces:**
- Consumes: `grammar::Check` (Task 2); the split of Task 1.
- Produces: `class inference::LlmBackend : public Tokenizer, public ChatModel, public Embedder {};` (no members of its own). `std::string llama::LlamaCppBackend::CheckGrammar(const std::string& grammar, std::string_view text)`, no longer an override. `app::ModelSeat::Check` fills `SeatCheck::problem` from `grammar::Check`. `test::ScriptedBackend` has no `SetGrammarVerdict`, `GrammarChecks` or `GrammarCheck`.

- [ ] **Step 1: Write the failing test**

In `tests/app/ModelSeatTests.cpp`, test case "ModelSeat publishes a request, checks a reply against its grammar, and answers it with Submit": delete the line

```cpp
    backend.SetGrammarVerdict("the grammar rejects 'maybe' at the start");
```

and replace the block

```cpp
    // Check asks the backend that published the request, with the request's grammar.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'maybe' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    REQUIRE(backend.GrammarChecks().size() == 1);
    CHECK(backend.GrammarChecks()[0].grammar == request.spec.grammar);
    CHECK(backend.GrammarChecks()[0].text == "maybe");
```

with

```cpp
    // Check fits the reply to the request's grammar with grammar::Check, and counts its tokens with the backend that
    // published the request.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'm' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    CHECK(seat.Check(published.id, "yes")->problem.empty());
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"`
Expected: FAIL in "ModelSeat publishes a request, ...": `CHECK( checked->problem == "the grammar rejects 'm' at the start" )` with expansion `"" == "the grammar rejects 'm' at the start"` (the scripted backend's default verdict).

- [ ] **Step 3: Check with grammar::Check and take CheckGrammar off the interface**

In `src/inference/Llm.h`, replace

```cpp
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
```

with

```cpp
// The engine's view of its models: all three parts (architecture spec, "Inference interface"). Code that needs one
// part can take that part alone. Whether a text fits a grammar is grammar::Check's job, independent of any backend.
class LlmBackend : public Tokenizer, public ChatModel, public Embedder {};
```

In `src/llama/LlamaCppBackend.h`, replace

```cpp
    // The grammar sampler alone over the text's tokens: no decode, so the KV cache is untouched.
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

with

```cpp
    // Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end, by
    // llama.cpp's own grammar sampler over the text's tokens: no decode, so the KV cache is untouched. Returns "" when
    // it is; otherwise a short reason naming the first rejected token and the text before it, or "the text ends
    // before the grammar does". Throws ContractError for an empty grammar or one that does not parse. Not part of
    // LlmBackend: the model-backed tests use it to confirm that grammar::Check agrees with llama.cpp.
    std::string CheckGrammar(const std::string& grammar, std::string_view text);
```

(`LlamaCppBackend.cpp`'s `CheckGrammar` body does not change.)

In `src/turn/Replay.h`, delete the line

```cpp
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

and in `src/turn/Replay.cpp` delete the definition

```cpp
std::string RecordingBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text); // A check is not a completion: nothing is recorded.
}

```

In `src/app/model/ModelSeatBackend.h`, delete the line

```cpp
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
```

and in `src/app/model/ModelSeatBackend.cpp` delete the definition

```cpp
std::string ModelSeatBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text);
}

```

In `src/app/model/ModelSeat.h`, replace

```cpp
    std::string problem = {}; // LlmBackend::CheckGrammar's verdict: "" when the reply fits the request's grammar.
```

with

```cpp
    std::string problem = {}; // grammar::Check's verdict: "" when the reply fits the request's grammar.
```

and replace the comment of `Check`

```cpp
    // Checks `text` against request `id`'s grammar with the backend that published it, and counts its tokens.
```

with

```cpp
    // Checks `text` against request `id`'s grammar (grammar::Check), and counts its tokens with the backend that
    // published the request.
```

In `src/app/model/ModelSeat.cpp`, add `#include "grammar/Check.h"` after `#include "core/Errors.h"`, and in `ModelSeat::Check` replace

```cpp
    // The worker is blocked in Await and cannot return from it while this lock is held, so the backend is idle.
    SeatCheck check;
    check.problem = _pBackend->CheckGrammar(_pending->spec.grammar, text);
    check.tokens = _pBackend->CountTokens(text);
```

with

```cpp
    // The worker is blocked in Await and cannot return from it while this lock is held, so the backend is idle.
    SeatCheck check;
    check.problem = grammar::Check(_pending->spec.grammar, text);
    check.tokens = _pBackend->CountTokens(text);
```

In `src/app/CMakeLists.txt`, the model library now calls `grammar` directly; replace

```cmake
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn)
```

with

```cmake
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn morpheus_grammar)
```

In `tests/fakes/FakeBackend.h`, replace the class comment's second sentence

```cpp
// message. Generation, the grammar check, embeddings and state are not used by those tests and throw if called.
```

with

```cpp
// message. Generation, embeddings and state are not used by those tests and throw if called.
```

and delete

```cpp
    std::string CheckGrammar(const std::string&, std::string_view) override
    {
        throw core::ContractError("FakeBackend::CheckGrammar is not available");
    }

```

In `tests/fakes/ScriptedBackend.h`, delete:
- the class comment's last line `// CheckGrammar records each check and answers the verdict SetGrammarVerdict set: "" (the text fits) by default.`;
- the nested struct `GrammarCheck` with its comment (`// One CheckGrammar call's arguments.` and the four lines of the struct);
- the members `std::string _grammarVerdict = {}; // What CheckGrammar answers.` and `std::vector<GrammarCheck> _grammarChecks = {};`;
- the methods `SetGrammarVerdict` and `GrammarChecks` with their comments;
- the override `std::string CheckGrammar(const std::string& grammar, std::string_view text) override { ... }` (five lines with its braces).

- [ ] **Step 4: Update the other tests that used the verdict hooks**

In `tests/app/ModelSeatTests.cpp`, test case "ModelSeat::Check needs a request with a grammar": delete the line `CHECK(backend.GrammarChecks().empty());`.

In `tests/app/ModelSeatBackendTests.cpp`, delete the five lines

```cpp
    scripted.SetGrammarVerdict("the grammar rejects 'no' at the start");
    CHECK(backend.CheckGrammar(R"(root ::= "yes")", "no") == "the grammar rejects 'no' at the start");
    REQUIRE(scripted.GrammarChecks().size() == 1);
    CHECK(scripted.GrammarChecks()[0].grammar == R"(root ::= "yes")");
    CHECK(scripted.GrammarChecks()[0].text == "no");
```

In `tests/turn/ReplayTests.cpp`, delete the whole test case "RecordingBackend forwards the grammar check and records nothing for it" (from its `TEST_CASE` line to its closing brace, and the blank line after it).

In `tests/app/UiTests.cpp`, test case "The Model seat window shows a Draft call's draft, checks it and submits it": replace

```cpp
    // The factory hands out the ScriptedBackend it made, to see the grammar checks reach it.
    std::atomic<test::ScriptedBackend*> pMade = nullptr;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder());
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        std::unique_ptr<inference::LlmBackend> pBackend = scripted(settings);
        pMade = static_cast<test::ScriptedBackend*>(pBackend.get());
        return pBackend;
    });
```

with

```cpp
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
```

and replace

```cpp
    // Check: the reply's token count and the grammar's verdict, from the real backend. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    REQUIRE(pMade.load() != nullptr);
    REQUIRE(pMade.load()->GrammarChecks().size() == 1);
    CHECK(pMade.load()->GrammarChecks()[0].text == MOVE);
    CHECK(controller.Seat().Pending());
```

with

```cpp
    // Check: the reply's token count by the real backend and grammar::Check's verdict. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    CHECK(controller.Seat().Pending());
    const std::optional<app::SeatCheck> checked = controller.Seat().Check(request.id, MOVE);
    REQUIRE(checked);
    CHECK(checked->problem == grammar::Check(request.spec.grammar, MOVE));
    CHECK(checked->tokens == static_cast<int>((MOVE.size() + 3) / 4)); // The scripted tokenizer: 4 bytes a token.
```

Add `#include "grammar/Check.h"` to `UiTests.cpp`'s project includes (after `#include "core/MemoryMode.h"`). If `<atomic>` is no longer used in `UiTests.cpp` (`grep -n "atomic" tests/app/UiTests.cpp` prints only the include), delete `#include <atomic>`.

Check: `grep -rn "CheckGrammar" src tests tools` prints only `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp` and `tests/llama/LlamaGrammarTests.cpp`; `grep -rn "GrammarVerdict\|GrammarChecks" tests` prints nothing.

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 374` (one test case deleted from `ReplayTests.cpp`).

- [ ] **Step 6: Build the dev targets and run the model-backed grammar tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported as in Task 2: `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"`
Expected: `All tests passed`.

- [ ] **Step 7: Byte check and commit**

```bash
check_bytes src/inference/Llm.h src/llama/LlamaCppBackend.h src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.h src/app/model/ModelSeatBackend.cpp src/app/CMakeLists.txt tests/fakes/FakeBackend.h tests/fakes/ScriptedBackend.h tests/app/ModelSeatTests.cpp tests/app/ModelSeatBackendTests.cpp tests/app/UiTests.cpp tests/turn/ReplayTests.cpp
git add src/inference/Llm.h src/llama/LlamaCppBackend.h src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.h src/app/model/ModelSeatBackend.cpp src/app/CMakeLists.txt tests/fakes/FakeBackend.h tests/fakes/ScriptedBackend.h tests/app/ModelSeatTests.cpp tests/app/ModelSeatBackendTests.cpp tests/app/UiTests.cpp tests/turn/ReplayTests.cpp
git commit -m "Take CheckGrammar off LlmBackend: the model seat checks with grammar::Check" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

