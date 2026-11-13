# Model Seat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A debug window in the normal app, "Model seat", in which the owner answers the engine's model calls by hand: each call shows its prompt, its token limits and its grammar, and the engine treats the owner's reply as the model's.

**Architecture:**
- **Inference:** `inference::LlmBackend` gains `CheckGrammar(grammar, text)`. `llama::LlamaCppBackend` runs llama.cpp's grammar sampler over the text's tokens without generating; `turn::RecordingBackend` forwards it; the test fakes implement it (`FakeBackend` refuses it like its other unused methods, `ScriptedBackend` answers a configurable verdict).
- **Shared state (`app/model`):** `app::ModelSeat` (`src/app/model/ModelSeat.h/.cpp`) holds the switch, one `app::SeatMode` per `app::SeatLabel`, the pending `app::SeatRequest` and the reply, under one mutex. `app::SeatLabelOf` labels a call from `turn::CallKindOf`. The worker waits in `ModelSeat::Await` on a `std::condition_variable_any` with the call's `std::stop_token`; `Submit`, `Withdraw` or the stop token wakes it. `Submit(id, ...)`, `Check(id, ...)` and `Withdraw(id)` name the request they mean by its id, so one that is no longer pending is a no-op that says so, never a crash.
- **Wrapper (`app/model`):** `app::ModelSeatBackend` (`src/app/model/ModelSeatBackend.h/.cpp`) owns the real backend and shares the seat. It forwards everything but `Complete`; `Complete` forwards while the seat is off or the label is in Model mode, and otherwise publishes the call (after a silent real call in Draft mode), waits, and builds the completion from the reply, cut to the call's limits by the real tokenizer.
- **Wiring:** `AppController` owns `std::shared_ptr<ModelSeat>` and wraps the factory it is given, so every backend the engine host builds (after restarts too) is a `ModelSeatBackend` around the same seat. `main.cpp` and `EngineHost` do not change.
- **UI:** `ModelSeatWindow` (`src/app/ui/ModelSeatWindow.cpp`), declared in `Windows.h` and drawn by `Ui`. Its **Withdraw** button releases any pending request: a call of a running turn or regenerate by cancelling the command (`AppController::Cancel`, so the turn closes with `TurnCancelled`), a fold or condense of between-turn maintenance by `ModelSeat::Withdraw` (the maintenance stops as a preempted one does). The Context window's prompt drawing becomes `DrawPrompt`, which both windows call. The status bar says when the engine waits for the seat.
- **Spec:** `docs/superpowers/specs/2026-11-13-model-seat-design.md` (the single source of requirements); architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022), CMake 3.25+, llama.cpp (dev preset), Dear ImGui 1.92.8 docking branch (headless in tests), Catch2 v3. No new dependencies.

**Choices this plan makes where the spec is silent or cannot be followed literally (the owner may overrule any of them):**
- **Unreachable summary check (spec contradiction).** The spec makes "a `Summarize` call whose last message ends with neither instruction" a `ContractError` and asks for a test of it. `turn::CallKindOf` returns `Summarize` only when the last message *equals* `SUMMARIZE_INSTRUCTION` or `CONDENSE_INSTRUCTION`, so such a call cannot exist and the test cannot be written. `SeatLabelOf` keeps the throw (it guards a future change of `CallKindOf`), and the test checks instead that a grammar-free call that merely ends with a quoted summary instruction is labelled `narrate`.
- **Cutting at the token limit.** The spec cuts the reply "at `spec.maxTokens` tokens, by the real tokenizer ... at a whole token's text". `LlmBackend` has no tokenize-to-pieces method (only `CountTokens`), and the spec adds only `CheckGrammar`. The wrapper keeps the longest prefix that ends on a UTF-8 character boundary and has `CountTokens(prefix) <= limit`, found by bisection over the character boundaries. It is valid UTF-8 and within the limit; it may not end exactly where the whole reply's own tokenization puts a token boundary.
- `SeatRequest::id` (1, 2, ... per seat; 0 is never issued) is the spec's: the window resets its edits when it changes, and passes it to `Submit`, `Check` and `Withdraw`. For an id in 1..last issued that is not the pending request (answered, cancelled or withdrawn), `Submit` and `Withdraw` return false and `Check` returns nullopt; an id never issued (0, or above the last) is a `ContractError`.
- Names the spec does not give: `SEAT_LABELS` (the seven labels in the window's order), `SeatLabelName` ("resolve", "narrate", "rewrite", "lookup", "review", "summarize", "condense"), `SeatReply {text, thinking}`, `SeatCheck {problem, tokens}` (Check's result), `ModelSeat::Await(request, backend, stop)` (the worker side), `MODEL_SEAT_WINDOW = "Model seat"`, class `ModelSeatWindow`, `DrawPrompt(prompt, showRaw)`, and the fake's `ScriptedBackend::SetGrammarVerdict` / `GrammarChecks`.
- `Submit` and `Withdraw` clear the pending request at once (the reply, or the withdrawal, waits for `Await` to take it), so `Pending()` is nullopt right after either, and a second Submit or Withdraw of the same id returns false.
- `Check` holds the seat's mutex while it calls the backend: the worker cannot leave `Await` until Check returns, so the backend is never used on two threads.
- `CheckGrammar` with an empty grammar is a `ContractError` in every implementation (llama.cpp would accept any text against an empty grammar, a silent "fits"). The reason strings are `the grammar rejects '<token>' at the start`, `the grammar rejects '<token>' after '<text>'` (the text before it cut to its last 60 bytes, `...` in front when cut) and `the text ends before the grammar does`. The end check uses the vocabulary's end-of-turn token, or end-of-sentence when it has none.
- In the seat path (Human or Draft), `Complete` keeps the interface's `ContractError`s for `maxTokens <= 0` and a negative thinking budget, but raises no context-overflow `InferenceError`: the spec's error section lists only the real model's and the tokenizer's errors.
- "Draft submitted unchanged" compares the thinking text only when the call has a thinking budget (with none, the thinking is ignored anyway). `generateMs` runs from publishing the request to `Await` returning (just after Submit). `onToken` is not called when the answer is empty (a model that writes nothing streams nothing).
- **Withdraw replaces Cancel turn** (one button, enabled whenever a request is pending). It decides by the run's busy state, not by the label: while a turn or regenerate runs (`RunModel::Busy::Turn` or `Regenerate`), it calls `AppController::Cancel`, exactly the Play window's Cancel, so a fold made by the turn's own fit loop, or by the maintenance that runs before a queued turn, is cancelled with that turn. With no command running (between-turn maintenance) it calls `ModelSeat::Withdraw(id)`. Checked against the engine: `SummarizeTurns` / `CondenseSummaries` return nullopt for any cancelled completion (no stop needed), `FoldOldest` / `Condense` then store nothing, `Session::Maintain` returns false having kept only the folds it finished, and the Worker treats that as a stop with nothing queued: no event, and maintenance is dropped until the next commit. The next turn's fit loop folds the over-budget transcript itself, so the withdrawn fold is attempted again there.
- The window draws, top to bottom: the switch, the modes table (Human / Draft / Model radio buttons per label), then for a pending call its label and limits, the model's thinking and the grammar (each a collapsed section), the reply box, the thinking box, the Check / Submit / Withdraw buttons with Check's result, and the prompt (Sections / Raw, as the Context window) filling the rest. It docks with State and Memory on the right in the default layout.
- The status bar text is `Waiting for the model seat: the <label> call`. It cannot be read back headless, so it is only drawn in a test.
- The headless UI tests press the window's buttons with ImGui's own `ImGui::ActivateItemByID` (`imgui_internal.h`), the programmatic activation ImGui provides; no test in the repo clicks yet.
- The architecture spec's "the worker is the only owner of the llama.cpp context" gains the one exception, Check, besides the Documentation section's two additions.
- The `[llama]` test runs from the configured `build/dev` tree, RelWithDebInfo (`build/dev/bin/RelWithDebInfo/morpheus_tests.exe`).
- The request ids close the Submit-after-cancel race: a Submit (or Check, or Withdraw) for a request the worker withdrew after a cancel, between the window's `Pending()` and its action, returns false (nullopt) instead of throwing.
- The window's Withdraw-on-maintenance branch is tested at the worker level (`ModelSeatBackend` around a `ScriptedBackend` under a `turn::Worker` with the session tests' folding budgets), not through the window: making an app run fold needs a small-context `settings.json` the settings loader would have to accept. The headless UI test clicks Withdraw on a turn's call.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25. No new dependencies.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /WX /permissive- /Zc:preprocessor /utf-8`): a warning fails the build.
- **Exact names (from the spec, verbatim):**
  - `virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;` on `inference::LlmBackend`, with the spec's comment.
  - `app::ModelSeat` in `src/app/model/ModelSeat.h/.cpp`; `app::ModelSeatBackend` in `src/app/model/ModelSeatBackend.h/.cpp`.
  - `enum class app::SeatLabel` with the seven values `Resolve`, `Narrate`, `Rewrite`, `Lookup`, `Review`, `Summarize`, `Condense`; `SeatLabelOf(prompt, spec)`.
  - `enum class app::SeatMode { Human, Draft, Model }`; every label starts in `Human`; the switch starts off.
  - `SeatRequest` with `id` (`std::uint64_t`), `label`, `prompt` (`inference::Prompt`), `spec` (`inference::GenerationSpec`), `draft` (`std::optional<inference::Completion>`).
  - `ModelSeat` methods `Enabled()`, `SetEnabled(bool)`, `ModeOf(SeatLabel)`, `SetMode(SeatLabel, SeatMode)`, `Pending()`, `bool Submit(id, text, thinking)`, `std::optional<SeatCheck> Check(id, text)`, `bool Withdraw(id)`.
  - `AppController::Seat()` returning `ModelSeat&`.
  - The window "Model seat" in `src/app/ui/ModelSeatWindow.cpp`, with an "Answer as the model" checkbox, the **Check**, **Submit** and **Withdraw** buttons, and "Fits the grammar".
- **Nothing in `settings.json`:** the seat's switch and modes live only in memory, and survive engine restarts.
- **No change to `core`, `context` or `turn` behaviour.** `morpheus_play` is out of scope. No replay recording of replies.
- **Error policy:** engine bugs throw `core::ContractError` via `MORPHEUS_REQUIRE`: `Submit`, `Check` or `Withdraw` with an id that was never published, `Check` on a call without a grammar, a second request while one is pending, an unrecognised summary call, a grammar that does not parse. A published id that is no longer pending is not a bug (a cancel or Withdraw can beat the window's click): `Submit` and `Withdraw` return false, `Check` returns nullopt. The owner's reply is never an exception: it goes to the engine like a model's. Real backend errors propagate as today. No fallbacks that hide a broken contract.
- **Module boundaries:** `app/model` has no SDL, ImGui or llama.cpp; `app/ui` needs only ImGui's core; only `src/llama` (and the tools) include `<llama.h>`. Dependencies flow downward only.
- **Coding rules (`CLAUDE.md`):** types and methods PascalCase, locals and parameters camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; constants ALL_CAPS and grouped; members initialized at declaration; class layout: nested types, public data, private data, public methods, private methods, helpers; comments let a method read top to bottom; a private helper only with more than one caller (local lambdas are fine); no legacy paths. ASCII only in source comments and docs.
- **Tests first:** every behavior lands with a Catch2 test written and run (failing) before the implementation.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash:
  - `cmake --build --preset tests`, `ctest --preset tests` (285 tests pass before this plan); one tag: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`.
  - The `dev` tree is configured in `build/dev`: `cmake --build build/dev --config RelWithDebInfo --target <target>`.
  - New test files are picked up by the next build (`CONFIGURE_DEPENDS`).
- **Never stage** `settings.json`, `models/`, `imgui.ini`, `docs/references/`, `.superpowers/` (the controller commits those notes) or `.gitmodules`. Stage the files each commit step names, nothing else.
- **Commits** are made on `master` (the owner consented). Every commit message ends with a `Co-Authored-By:` line naming the implementing model. The steps show `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; an implementer running as Opus writes `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` instead.

## File Structure

```
src/inference/Llm.h                        modify: LlmBackend::CheckGrammar (Task 1)
src/llama/LlamaCppBackend.h/.cpp           modify: CheckGrammar with the grammar sampler (Task 1)
src/turn/Replay.h/.cpp                     modify: RecordingBackend forwards CheckGrammar (Task 1)
tests/fakes/FakeBackend.h                  modify: CheckGrammar refuses (Task 1)
tests/fakes/ScriptedBackend.h              modify: CheckGrammar, SetGrammarVerdict, GrammarChecks (Task 1)
src/app/model/ModelSeat.h/.cpp             create: SeatLabel, SeatMode, SeatLabelOf, SeatRequest, ModelSeat (Task 2)
src/app/model/ModelSeatBackend.h/.cpp      create: the wrapper (Task 3)
src/app/model/AppController.h/.cpp         modify: the seat, the wrapped factory, Seat() (Task 4)
src/app/ui/Windows.h                       modify: MODEL_SEAT_WINDOW, DrawPrompt, ModelSeatWindow (Task 5)
src/app/ui/ModelSeatWindow.cpp             create: the window (Task 5)
src/app/ui/ContextWindow.cpp               modify: draws its prompt with DrawPrompt (Task 5)
src/app/ui/Ui.h/.cpp                       modify: DrawPrompt, the window, its dock place, the status bar (Task 5)
src/app/CMakeLists.txt                     modify: the three new sources (Tasks 2, 3, 5)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, README.md   modify (Task 6)
tests/turn/ReplayTests.cpp                 modify (Task 1)
tests/llama/LlamaGrammarTests.cpp          modify (Task 1, dev preset only)
tests/app/SeatTestSupport.h                create: AsyncCall, WaitForRequest, SeatOnlyFor, Streamed (Task 2)
tests/app/ModelSeatTests.cpp               create (Task 2)
tests/app/ModelSeatBackendTests.cpp        create (Task 3)
tests/app/AppControllerTests.cpp           modify (Task 4)
tests/app/UiTests.cpp                      modify (Task 5)
```

---

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

### Task 2: The seat's labels, modes and shared state

`SeatLabelOf` labels a call from `turn::CallKindOf`. `ModelSeat` holds the switch, the modes, the pending request and the reply under one mutex; the worker side, `Await`, publishes a request and waits on a `std::condition_variable_any` with the call's stop token, until `Submit`, `Withdraw` or the stop. The UI's `Submit`, `Check` and `Withdraw` name their request by id.

**Files:**
- Create: `src/app/model/ModelSeat.h`, `src/app/model/ModelSeat.cpp`, `tests/app/SeatTestSupport.h`, `tests/app/ModelSeatTests.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `turn::CallKindOf`, `turn::RENARRATE_CORRECTION`, `turn::SUMMARIZE_INSTRUCTION`, `turn::CONDENSE_INSTRUCTION`; `LlmBackend::CheckGrammar` and `test::ScriptedBackend::SetGrammarVerdict` / `GrammarChecks` (Task 1).
- Produces (namespace `morpheus::app`, header `app/model/ModelSeat.h`):
  - `enum class SeatLabel { Resolve, Narrate, Rewrite, Lookup, Review, Summarize, Condense };`
  - `enum class SeatMode { Human, Draft, Model };`
  - `constexpr std::array<SeatLabel, 7> SEAT_LABELS` (the order above); `std::string SeatLabelName(SeatLabel label)`; `SeatLabel SeatLabelOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)`.
  - `struct SeatRequest { std::uint64_t id; SeatLabel label; inference::Prompt prompt; inference::GenerationSpec spec; std::optional<inference::Completion> draft; };`
  - `struct SeatReply { std::string text; std::string thinking; };`, `struct SeatCheck { std::string problem; int tokens; };`
  - `class ModelSeat` with `bool Enabled() const`, `void SetEnabled(bool)`, `SeatMode ModeOf(SeatLabel) const`, `void SetMode(SeatLabel, SeatMode)`, `std::optional<SeatRequest> Pending() const`, `bool Submit(std::uint64_t id, std::string text, std::string thinking)`, `std::optional<SeatCheck> Check(std::uint64_t id, std::string_view text)`, `bool Withdraw(std::uint64_t id)`, `std::optional<SeatReply> Await(SeatRequest request, inference::LlmBackend& backend, std::stop_token stop)` (nullopt after a stop or a Withdraw).
  - Test helpers (namespace `morpheus::test`, header `tests/app/SeatTestSupport.h`): `template <typename Result> class AsyncCall` (constructor taking a callable `Result(std::stop_token)`, `Stop()`, `Get()`), `app::SeatRequest WaitForRequest(const app::ModelSeat&)`, `void SeatOnlyFor(app::ModelSeat&, app::SeatLabel, app::SeatMode)`, `class Streamed` (`Callback()`, `Pieces()`). Tasks 3 to 5 use all of them.

- [ ] **Step 1: Write the test support header**

Create `tests/app/SeatTestSupport.h`:

```cpp
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/model/ModelSeat.h"
#include "inference/Llm.h"

namespace morpheus::test {

// Runs `call(stop)` on another thread, as the engine worker makes a model call. The destructor requests the stop and
// waits for the call, so a test that fails while the call waits at the seat ends instead of hanging.
template <typename Result>
class AsyncCall {
private:
    std::stop_source _stop = {}; // Before _result: the call's lambda takes its token.
    std::future<Result> _result = {};

public:
    template <typename Call>
    explicit AsyncCall(Call call)
        : _result(std::async(std::launch::async,
                             [call = std::move(call), stop = _stop.get_token()]() mutable { return call(stop); }))
    {
    }
    ~AsyncCall()
    {
        _stop.request_stop();
        if (_result.valid()) {
            _result.wait();
        }
    }
    AsyncCall(const AsyncCall&) = delete;
    AsyncCall& operator=(const AsyncCall&) = delete;

    // Requests the call's stop, as Worker::Cancel does.
    void Stop()
    {
        _stop.request_stop();
    }

    // The call's result, rethrowing what it threw. Fails the test when the call has not returned within 10 s.
    Result Get()
    {
        REQUIRE(_result.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        return _result.get();
    }
};

// Polls `seat` until a request is pending, as the window does each frame, and returns a copy of it. Fails the test
// after 10 s.
inline app::SeatRequest WaitForRequest(const app::ModelSeat& seat)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        if (std::optional<app::SeatRequest> request = seat.Pending()) {
            return std::move(*request);
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Turns the seat on with `label` in `mode` and every other label in Model mode.
inline void SeatOnlyFor(app::ModelSeat& seat, app::SeatLabel label, app::SeatMode mode)
{
    seat.SetEnabled(true);
    for (const app::SeatLabel each : app::SEAT_LABELS) {
        seat.SetMode(each, each == label ? mode : app::SeatMode::Model);
    }
}

// Collects what a Complete streams to its token callback, from whichever thread it runs on.
class Streamed {
private:
    std::mutex _mutex = {};
    std::vector<std::string> _pieces = {};

public:
    // A callback that appends each piece. This object must outlive the call it is given to.
    inference::TokenCallback Callback()
    {
        return [this](std::string_view piece) {
            std::scoped_lock lock(_mutex);
            _pieces.emplace_back(piece);
        };
    }

    std::vector<std::string> Pieces()
    {
        std::scoped_lock lock(_mutex);
        return _pieces;
    }
};

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing tests**

Create `tests/app/ModelSeatTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "TestPaths.h"
#include "app/SeatTestSupport.h"
#include "app/model/ModelSeat.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "fakes/FakeBackend.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"
#include "turn/EngineText.h"
#include "turn/Folding.h"
#include "turn/Resolve.h"
#include "turn/ResolveGrammar.h"
#include "turn/Review.h"

using namespace morpheus;

namespace {

// A grammar-free request, as for a narrate call.
app::SeatRequest NarrateRequest()
{
    app::SeatRequest request;
    request.label = app::SeatLabel::Narrate;
    request.prompt = {{{inference::Role::User, "I walk."}}, 0};
    return request;
}

} // namespace

TEST_CASE("SeatLabelOf labels each call the way the session and folding make it", "[model_seat]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    test::FakeBackend backend(16384);
    const inference::Message system = {inference::Role::System, "story"};
    const inference::GenerationSpec plain; // Narration and summaries have no grammar.

    // The resolve and lookup calls, told apart by their grammars.
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted);
    inference::GenerationSpec lookupSpec;
    lookupSpec.grammar = turn::BuildLookupGrammar();
    const inference::Prompt turnPrompt = {{system, {inference::Role::User, "I climb the cliff."}}, 1};
    CHECK(app::SeatLabelOf(turnPrompt, resolveSpec) == app::SeatLabel::Resolve);
    CHECK(app::SeatLabelOf(turnPrompt, lookupSpec) == app::SeatLabel::Lookup);

    // The narrate call, and the renarrate, whose message carries the review's correction.
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(R"({"intent":"climb","checks":[],"changes":[]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const inference::Prompt narrate = {
        {system, {inference::Role::User, turn::RenderNarrateMessage(proposal, resolution, {}, 4096, backend)}}, 1};
    const inference::Prompt rewrite = {
        {system,
         {inference::Role::User,
          turn::RenderNarrateMessage(proposal, resolution, {"The narration has the player fly."}, 4096, backend)}},
        1};
    CHECK(app::SeatLabelOf(narrate, plain) == app::SeatLabel::Narrate);
    CHECK(app::SeatLabelOf(rewrite, plain) == app::SeatLabel::Rewrite);

    // The review: its own prompt and grammar.
    inference::GenerationSpec reviewSpec;
    reviewSpec.grammar = turn::BuildReviewGrammar(true, true);
    CHECK(app::SeatLabelOf(turn::ReviewPrompt("I climb the cliff.", proposal, resolution, "You climb."), reviewSpec) ==
          app::SeatLabel::Review);

    // The fold and condense calls.
    core::TurnRecord played;
    played.number = 1;
    played.input = "I climb the cliff.";
    played.narration = "You climb.";
    CHECK(app::SeatLabelOf(turn::SummarizePrompt(system, {played}), plain) == app::SeatLabel::Summarize);
    CHECK(app::SeatLabelOf(turn::CondensePrompt(system), plain) == app::SeatLabel::Condense);

    // CallKindOf takes a summary call to end with exactly the instruction, so a grammar-free call that only quotes
    // one is narration. (A summary call that ends with neither instruction cannot be built: SeatLabelOf's throw for
    // it guards a change of CallKindOf.)
    const inference::Prompt quoting = {
        {system, {inference::Role::User, "You were told: " + std::string(turn::SUMMARIZE_INSTRUCTION)}}, 1};
    CHECK(app::SeatLabelOf(quoting, plain) == app::SeatLabel::Narrate);
}

TEST_CASE("SeatLabelName spells the seven labels in the window's order", "[model_seat]")
{
    std::vector<std::string> names;
    for (const app::SeatLabel label : app::SEAT_LABELS) {
        names.push_back(app::SeatLabelName(label));
    }
    CHECK(names ==
          std::vector<std::string>{"resolve", "narrate", "rewrite", "lookup", "review", "summarize", "condense"});
}

TEST_CASE("ModelSeat starts off with every label in Human mode, and keeps what the window sets", "[model_seat]")
{
    app::ModelSeat seat;
    CHECK_FALSE(seat.Enabled());
    for (const app::SeatLabel label : app::SEAT_LABELS) {
        CHECK(seat.ModeOf(label) == app::SeatMode::Human);
    }
    CHECK_FALSE(seat.Pending());

    seat.SetEnabled(true);
    seat.SetMode(app::SeatLabel::Review, app::SeatMode::Draft);
    seat.SetMode(app::SeatLabel::Condense, app::SeatMode::Model);
    CHECK(seat.Enabled());
    CHECK(seat.ModeOf(app::SeatLabel::Review) == app::SeatMode::Draft);
    CHECK(seat.ModeOf(app::SeatLabel::Condense) == app::SeatMode::Model);
    CHECK(seat.ModeOf(app::SeatLabel::Resolve) == app::SeatMode::Human);
}

TEST_CASE("ModelSeat refuses Submit, Check and Withdraw for a request it never published", "[model_seat]")
{
    app::ModelSeat seat;
    // No request was ever published: every id is unknown, 0 included (ids start at 1).
    CHECK_THROWS_AS(seat.Submit(1, "yes", ""), core::ContractError);
    CHECK_THROWS_AS(seat.Check(1, "yes"), core::ContractError);
    CHECK_THROWS_AS(seat.Withdraw(1), core::ContractError);
    CHECK_THROWS_AS(seat.Submit(0, "yes", ""), core::ContractError);
}

TEST_CASE("ModelSeat publishes a request, checks a reply against its grammar, and answers it with Submit",
          "[model_seat]")
{
    app::ModelSeat seat;
    test::ScriptedBackend backend(16384);
    backend.SetGrammarVerdict("the grammar rejects 'maybe' at the start");
    app::SeatRequest request;
    request.label = app::SeatLabel::Resolve;
    request.prompt = {{{inference::Role::User, "Is it yes?"}}, 0};
    request.spec.grammar = R"(root ::= "yes" | "no")";
    request.spec.maxTokens = 8;
    test::AsyncCall<std::optional<app::SeatReply>> waiting(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });

    const app::SeatRequest published = test::WaitForRequest(seat);
    CHECK(published.id == 1);
    CHECK(published.label == app::SeatLabel::Resolve);
    CHECK(published.spec.grammar == request.spec.grammar);
    CHECK(published.spec.maxTokens == 8);
    REQUIRE(published.prompt.messages.size() == 1);
    CHECK(published.prompt.messages[0].content == "Is it yes?");
    CHECK_FALSE(published.draft);

    // Check asks the backend that published the request, with the request's grammar.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'maybe' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    REQUIRE(backend.GrammarChecks().size() == 1);
    CHECK(backend.GrammarChecks()[0].grammar == request.spec.grammar);
    CHECK(backend.GrammarChecks()[0].text == "maybe");

    // One request at a time: a second one while this one waits is an engine bug, and the first stays.
    CHECK_THROWS_AS(seat.Await(request, backend, std::stop_token()), core::ContractError);
    REQUIRE(seat.Pending());
    CHECK(seat.Pending()->id == 1);
    // An id the seat has not issued yet is an engine bug too.
    CHECK_THROWS_AS(seat.Submit(2, "no", ""), core::ContractError);

    // Submit answers the request, which is no longer pending. Its id is then stale: a second Submit, a Check or a
    // Withdraw of it does nothing and says so.
    CHECK(seat.Submit(published.id, "yes", "It reads like a yes."));
    CHECK_FALSE(seat.Pending());
    CHECK_FALSE(seat.Submit(published.id, "no", ""));
    CHECK_FALSE(seat.Check(published.id, "no"));
    CHECK_FALSE(seat.Withdraw(published.id));
    const std::optional<app::SeatReply> reply = waiting.Get();
    REQUIRE(reply);
    CHECK(reply->text == "yes");
    CHECK(reply->thinking == "It reads like a yes.");

    // The next request gets the next id, and the old one stays stale while it waits.
    test::AsyncCall<std::optional<app::SeatReply>> next(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });
    const app::SeatRequest second = test::WaitForRequest(seat);
    CHECK(second.id == 2);
    CHECK_FALSE(seat.Submit(published.id, "stale", ""));
    CHECK(seat.Pending());
    CHECK(seat.Submit(second.id, "no", ""));
    CHECK(next.Get()->text == "no");
}

TEST_CASE("ModelSeat::Check needs a request with a grammar", "[model_seat]")
{
    app::ModelSeat seat;
    test::ScriptedBackend backend(16384);
    const app::SeatRequest request = NarrateRequest();
    test::AsyncCall<std::optional<app::SeatReply>> waiting(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });

    const app::SeatRequest published = test::WaitForRequest(seat);
    CHECK_THROWS_AS(seat.Check(published.id, "You walk."), core::ContractError);
    CHECK(backend.GrammarChecks().empty());
    CHECK(seat.Submit(published.id, "You walk.", ""));
    CHECK(waiting.Get());
}

TEST_CASE("A stop wakes a request waiting at the seat and clears it; a Submit after it is stale", "[model_seat]")
{
    app::ModelSeat seat;
    test::ScriptedBackend backend(16384);
    const app::SeatRequest request = NarrateRequest();
    test::AsyncCall<std::optional<app::SeatReply>> waiting(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });

    const app::SeatRequest published = test::WaitForRequest(seat);
    waiting.Stop();
    CHECK_FALSE(waiting.Get());
    CHECK_FALSE(seat.Pending());
    // The window's click that lost the race with the cancel: nothing happens, nothing throws.
    bool submitted = true;
    CHECK_NOTHROW(submitted = seat.Submit(published.id, "Too late.", ""));
    CHECK_FALSE(submitted);
    CHECK_FALSE(seat.Check(published.id, "Too late."));

    // A stop requested before the request is published returns at once.
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(seat.Await(request, backend, stopped.get_token()));
    CHECK_FALSE(seat.Pending());
}

TEST_CASE("Withdraw wakes a request waiting at the seat as a stop does", "[model_seat]")
{
    app::ModelSeat seat;
    test::ScriptedBackend backend(16384);
    const app::SeatRequest request = NarrateRequest();
    test::AsyncCall<std::optional<app::SeatReply>> waiting(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });

    const app::SeatRequest published = test::WaitForRequest(seat);
    CHECK(seat.Withdraw(published.id));
    CHECK_FALSE(seat.Pending()); // Gone at once, before the worker wakes.
    CHECK_FALSE(waiting.Get()); // The wait returns as cancelled; no stop was requested.
    CHECK_FALSE(seat.Withdraw(published.id));
    CHECK_FALSE(seat.Submit(published.id, "Too late.", ""));

    // The seat takes the next request as usual.
    test::AsyncCall<std::optional<app::SeatReply>> next(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });
    const app::SeatRequest second = test::WaitForRequest(seat);
    CHECK(second.id == 2);
    CHECK(seat.Submit(second.id, "You walk.", ""));
    CHECK(next.Get()->text == "You walk.");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'app/model/ModelSeat.h'`.

- [ ] **Step 4: Write the header**

Create `src/app/model/ModelSeat.h`:

```cpp
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "inference/Llm.h"

namespace morpheus::app {

// Which of the engine's model calls the model seat is asked to answer (the model seat design, "Call labels and
// modes"). Each label has its own mode.
enum class SeatLabel { Resolve, Narrate, Rewrite, Lookup, Review, Summarize, Condense };

// Who answers a label's calls while the seat is on.
// - Human: the owner writes the reply; the real model is not asked.
// - Draft: the real model answers first, and the owner submits its answer unchanged or edited.
// - Model: the real model answers and nothing waits, as with the seat off.
enum class SeatMode { Human, Draft, Model };
static_assert(static_cast<int>(SeatMode::Human) == 0, "ModelSeat::_modes value-initializes every label to Human");

// Every label, in the window's order.
constexpr std::array<SeatLabel, 7> SEAT_LABELS = {SeatLabel::Resolve, SeatLabel::Narrate,   SeatLabel::Rewrite,
                                                  SeatLabel::Lookup,  SeatLabel::Review,    SeatLabel::Summarize,
                                                  SeatLabel::Condense};

// The label's spelling in the window: "resolve", "narrate", "rewrite", "lookup", "review", "summarize" or "condense".
std::string SeatLabelName(SeatLabel label);

// The label of a call, from its prompt and spec as turn::CallKindOf tells them apart: a narrate call whose last
// message contains turn::RENARRATE_CORRECTION is a rewrite, and a summary call is a summarize or a condense by the
// instruction its last message ends with. Throws ContractError for a summary call that ends with neither (the engine
// makes no such call).
SeatLabel SeatLabelOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec);

// A call waiting for the owner's reply.
struct SeatRequest {
    std::uint64_t id = 0; // 1 for the seat's first request, one more for each: tells a new request from the last.
    SeatLabel label = SeatLabel::Resolve;
    inference::Prompt prompt = {};
    inference::GenerationSpec spec = {}; // maxTokens, thinkingMaxTokens and grammar: what the model is held to.
    std::optional<inference::Completion> draft = {}; // The real model's answer, in Draft mode.
};

// The owner's reply to a request.
struct SeatReply {
    std::string text = {};
    std::string thinking = {};
};

// What Check found about a reply.
struct SeatCheck {
    std::string problem = {}; // LlmBackend::CheckGrammar's verdict: "" when the reply fits the request's grammar.
    int tokens = 0;           // The reply's tokens, by the real tokenizer.
};

// The model seat's state (the model seat design), shared by the UI thread (the Model seat window) and the engine
// worker thread (ModelSeatBackend). A mutex guards every member. The worker runs one job at a time, so at most one
// request is pending, and while one is the worker is blocked in Await.
//
// The UI names the request it acts on by its id. A request ends when it is submitted, withdrawn or stopped, and may
// end between the window's Pending() and its action (a cancel wakes the worker on its own thread): an id issued
// before but no longer pending is stale, and Submit, Check and Withdraw then do nothing and say so. An id never
// issued is an engine bug.
class ModelSeat {
private:
    mutable std::mutex _mutex = {};
    std::condition_variable_any _woken = {};             // Notified by Submit and Withdraw; Await also wakes on its stop.
    bool _enabled = false;                               // The window's "Answer as the model".
    std::array<SeatMode, SEAT_LABELS.size()> _modes = {}; // Indexed by SeatLabel; value-initialized: all Human.
    std::optional<SeatRequest> _pending = {};
    std::optional<SeatReply> _reply = {};       // Submit's reply, until Await takes it.
    bool _withdrawn = false;                    // Withdraw released the pending request, until Await sees it.
    inference::LlmBackend* _pBackend = nullptr; // The pending request's real backend, for Check. Not owned.
    std::uint64_t _lastId = 0;                  // The latest request's id; 0 before the first.

public:
    // The switch. Turning it off leaves a pending request waiting: only later calls go to the model.
    bool Enabled() const;
    void SetEnabled(bool enabled);

    SeatMode ModeOf(SeatLabel label) const;
    void SetMode(SeatLabel label, SeatMode mode);

    // A copy of the pending request, or nullopt.
    std::optional<SeatRequest> Pending() const;

    // Answers request `id` with `text` and `thinking`; it is no longer pending. Returns false, doing nothing, when
    // `id` is stale. Throws ContractError for an id never issued.
    bool Submit(std::uint64_t id, std::string text, std::string thinking);

    // Checks `text` against request `id`'s grammar with the backend that published it, and counts its tokens.
    // Returns nullopt when `id` is stale. Throws ContractError for an id never issued, or when the request has no
    // grammar. The worker is blocked in Await while a request is pending and cannot return from it while Check holds
    // the lock, so the backend is not in use on another thread.
    std::optional<SeatCheck> Check(std::uint64_t id, std::string_view text);

    // Releases request `id` without a reply: Await returns as it does for a stop, and the call ends cancelled. It is
    // no longer pending. Returns false, doing nothing, when `id` is stale. Throws ContractError for an id never
    // issued.
    bool Withdraw(std::uint64_t id);

    // The worker's side (ModelSeatBackend::Complete). Publishes `request` (its id is assigned here) with `backend`
    // for Check, then waits for Submit, Withdraw or `stop`. Returns the reply, or nullopt after a Withdraw or a stop;
    // either way no request is pending afterwards. Throws ContractError when a request is already pending.
    std::optional<SeatReply> Await(SeatRequest request, inference::LlmBackend& backend, std::stop_token stop);
};

} // namespace morpheus::app
```

- [ ] **Step 5: Write the implementation**

Create `src/app/model/ModelSeat.cpp`:

```cpp
#include "app/model/ModelSeat.h"

#include <cstddef>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "turn/EngineText.h"
#include "turn/Replay.h"

namespace morpheus::app {

std::string SeatLabelName(SeatLabel label)
{
    switch (label) {
    case SeatLabel::Resolve:
        return "resolve";
    case SeatLabel::Narrate:
        return "narrate";
    case SeatLabel::Rewrite:
        return "rewrite";
    case SeatLabel::Lookup:
        return "lookup";
    case SeatLabel::Review:
        return "review";
    case SeatLabel::Summarize:
        return "summarize";
    case SeatLabel::Condense:
        return "condense";
    }
    throw core::ContractError("SeatLabelName: unhandled SeatLabel");
}

SeatLabel SeatLabelOf(const inference::Prompt& prompt, const inference::GenerationSpec& spec)
{
    // The call's kind, as the replay recorder tells it; narration and summaries are then split in two.
    switch (turn::CallKindOf(prompt, spec)) {
    case core::CallKind::Resolve:
        return SeatLabel::Resolve;
    case core::CallKind::Lookup:
        return SeatLabel::Lookup;
    case core::CallKind::Review:
        return SeatLabel::Review;
    case core::CallKind::Narrate: {
        // A renarrate is a narrate call whose message carries the review's correction.
        const bool rewrite = !prompt.messages.empty() &&
                             prompt.messages.back().content.find(turn::RENARRATE_CORRECTION) != std::string::npos;
        return rewrite ? SeatLabel::Rewrite : SeatLabel::Narrate;
    }
    case core::CallKind::Summarize: {
        // A fold ends with the summarize instruction, a condense with its own. CallKindOf returns Summarize only for
        // a prompt with messages.
        const std::string& last = prompt.messages.back().content;
        if (last.ends_with(turn::SUMMARIZE_INSTRUCTION)) {
            return SeatLabel::Summarize;
        }
        if (last.ends_with(turn::CONDENSE_INSTRUCTION)) {
            return SeatLabel::Condense;
        }
        throw core::ContractError("SeatLabelOf: a summary call ends with neither the summarize nor the condense "
                                  "instruction");
    }
    case core::CallKind::Renarrate:
        break; // CallKindOf never returns it: a renarrate is a narrate call, told apart above.
    }
    throw core::ContractError("SeatLabelOf: unhandled call kind");
}

bool ModelSeat::Enabled() const
{
    std::scoped_lock lock(_mutex);
    return _enabled;
}

void ModelSeat::SetEnabled(bool enabled)
{
    std::scoped_lock lock(_mutex);
    _enabled = enabled; // A pending request is not touched: it waits until it is submitted or cancelled.
}

SeatMode ModelSeat::ModeOf(SeatLabel label) const
{
    std::scoped_lock lock(_mutex);
    return _modes[static_cast<std::size_t>(label)];
}

void ModelSeat::SetMode(SeatLabel label, SeatMode mode)
{
    std::scoped_lock lock(_mutex);
    _modes[static_cast<std::size_t>(label)] = mode;
}

std::optional<SeatRequest> ModelSeat::Pending() const
{
    std::scoped_lock lock(_mutex);
    return _pending;
}

bool ModelSeat::Submit(std::uint64_t id, std::string text, std::string thinking)
{
    {
        std::scoped_lock lock(_mutex);
        MORPHEUS_REQUIRE(id >= 1 && id <= _lastId, "ModelSeat: Submit for a request the seat never published");
        if (!_pending || _pending->id != id) {
            return false; // Stale: answered, withdrawn or cancelled already.
        }
        // The reply waits for Await to take it. The request is answered, so it is no longer pending: the window
        // stops showing it at once, and a second Submit of it is stale.
        _reply = SeatReply{std::move(text), std::move(thinking)};
        _pending.reset();
        _pBackend = nullptr;
    }
    _woken.notify_all();
    return true;
}

std::optional<SeatCheck> ModelSeat::Check(std::uint64_t id, std::string_view text)
{
    std::scoped_lock lock(_mutex);
    MORPHEUS_REQUIRE(id >= 1 && id <= _lastId, "ModelSeat: Check for a request the seat never published");
    if (!_pending || _pending->id != id) {
        return std::nullopt; // Stale: nothing left to check against.
    }
    MORPHEUS_REQUIRE(!_pending->spec.grammar.empty(), "ModelSeat: Check on a call without a grammar");
    // The worker is blocked in Await and cannot return from it while this lock is held, so the backend is idle.
    SeatCheck check;
    check.problem = _pBackend->CheckGrammar(_pending->spec.grammar, text);
    check.tokens = _pBackend->CountTokens(text);
    return check;
}

bool ModelSeat::Withdraw(std::uint64_t id)
{
    {
        std::scoped_lock lock(_mutex);
        MORPHEUS_REQUIRE(id >= 1 && id <= _lastId, "ModelSeat: Withdraw for a request the seat never published");
        if (!_pending || _pending->id != id) {
            return false; // Stale: answered, withdrawn or cancelled already.
        }
        // Await sees the flag and returns as for a stop; the request is no longer pending from now on.
        _withdrawn = true;
        _pending.reset();
        _pBackend = nullptr;
    }
    _woken.notify_all();
    return true;
}

std::optional<SeatReply> ModelSeat::Await(SeatRequest request, inference::LlmBackend& backend, std::stop_token stop)
{
    std::unique_lock lock(_mutex);
    MORPHEUS_REQUIRE(!_pending.has_value() && !_reply.has_value() && !_withdrawn,
                     "ModelSeat: a second request while one is pending");
    // Publish: the window shows the request from its next frame, and Check may use the backend.
    request.id = ++_lastId;
    _pending = std::move(request);
    _pBackend = &backend;

    // Wait for Submit or Withdraw, or for the stop token (a Cancel, or a command preempting maintenance), which wakes
    // the wait too.
    const bool woken = _woken.wait(lock, stop, [this] { return _reply.has_value() || _withdrawn; });

    // Answered, withdrawn or stopped, the request is over (Submit and Withdraw have already cleared it).
    _pending.reset();
    _pBackend = nullptr;
    const bool withdrawn = std::exchange(_withdrawn, false);
    std::optional<SeatReply> reply = std::move(_reply);
    _reply.reset();
    if (!woken || withdrawn) {
        return std::nullopt; // Ends as a cancelled call. (A reply cannot exist here: Submit refuses a withdrawn id.)
    }
    return reply;
}

} // namespace morpheus::app
```

- [ ] **Step 6: Add the source to the build**

In `src/app/CMakeLists.txt`, replace

```cmake
    model/Inspect.cpp
    model/RunModel.cpp
```

with

```cmake
    model/Inspect.cpp
    model/ModelSeat.cpp
    model/RunModel.cpp
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"`
Expected: all 8 `[model_seat]` test cases pass.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 294`.

- [ ] **Step 8: Commit**

```bash
git add src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/CMakeLists.txt tests/app/SeatTestSupport.h tests/app/ModelSeatTests.cpp
git commit -m "Add the model seat's labels, modes and shared state" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: ModelSeatBackend, the wrapper

The wrapper forwards everything but `Complete`. `Complete` forwards while the seat is off or the call's label is in Model mode; otherwise it publishes the call (after a silent real call in Draft mode), waits, and returns the reply as the model's completion, or a cancelled completion after a stop or a Withdraw. A worker-level test shows that withdrawing a maintenance fold ends that maintenance cleanly and that the next turn folds again.

**Files:**
- Create: `src/app/model/ModelSeatBackend.h`, `src/app/model/ModelSeatBackend.cpp`, `tests/app/ModelSeatBackendTests.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `ModelSeat` (`Enabled`, `ModeOf`, `Await`, and in the tests `Submit(id, ...)`, `Withdraw(id)`), `SeatLabelOf`, `SeatRequest`, `SeatReply` (Task 2); `LlmBackend::CheckGrammar` (Task 1); test helpers `AsyncCall`, `WaitForRequest`, `SeatOnlyFor`, `Streamed` (Task 2); `turn::Worker` (existing).
- Produces: `class app::ModelSeatBackend final : public inference::LlmBackend` with `ModelSeatBackend(std::unique_ptr<inference::LlmBackend> pInner, std::shared_ptr<ModelSeat> pSeat)` (ContractError when either is null) and every `LlmBackend` override.

- [ ] **Step 1: Write the failing tests**

Create `tests/app/ModelSeatBackendTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "TestPaths.h"
#include "app/SeatTestSupport.h"
#include "app/model/ModelSeat.h"
#include "app/model/ModelSeatBackend.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/FakeBackend.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"
#include "turn/EngineText.h"
#include "turn/Worker.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A narrate call, as the seat labels it: no grammar, and a last message that is neither a summary instruction nor
// a renarrate's correction.
const inference::Prompt NARRATE_PROMPT = {{{inference::Role::System, "story"}, {inference::Role::User, "I walk."}}, 1};

const std::string MODEL_REPLY = "The model walks.";
const std::string MODEL_THOUGHT = "Model thoughts.";
const std::string MOVE =
    R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";

// The events of one worker command, through its closing event. Fails the test if any wait exceeds 10 s.
std::vector<turn::Event> UntilClosing(turn::Worker& worker)
{
    std::vector<turn::Event> events;
    for (;;) {
        std::optional<turn::Event> event = worker.WaitEvent(std::chrono::seconds(10));
        REQUIRE(event);
        events.push_back(std::move(*event));
        if (turn::IsClosing(events.back())) {
            return events;
        }
    }
}

// The real model of these tests: a ScriptedBackend that answers every call with MODEL_REPLY and thinks
// MODEL_THOUGHT. It counts one token per 4 bytes, rounded up.
std::unique_ptr<test::ScriptedBackend> MakeScripted()
{
    auto pScripted = std::make_unique<test::ScriptedBackend>(
        16384, [](const inference::Prompt&, const inference::GenerationSpec&) { return MODEL_REPLY; });
    pScripted->SetThinker([](const inference::Prompt&, const inference::GenerationSpec&) { return MODEL_THOUGHT; });
    return pScripted;
}

} // namespace

TEST_CASE("ModelSeatBackend forwards everything but Complete to the real backend", "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    pSeat->SetEnabled(true); // Forwarding does not depend on the switch.

    CHECK(backend.CountTokens("twelve bytes") == scripted.CountTokens("twelve bytes"));
    CHECK(backend.CountPromptTokens(NARRATE_PROMPT) == scripted.CountPromptTokens(NARRATE_PROMPT));
    CHECK(backend.ContextSize() == 16384);
    CHECK(backend.Embed({"the lighthouse keeper"}, inference::EmbedPurpose::Query) ==
          scripted.Embed({"the lighthouse keeper"}, inference::EmbedPurpose::Query));
    CHECK(backend.EmbeddingModelId() == "scripted-embed");
    scripted.SetGrammarVerdict("the grammar rejects 'no' at the start");
    CHECK(backend.CheckGrammar(R"(root ::= "yes")", "no") == "the grammar rejects 'no' at the start");
    REQUIRE(scripted.GrammarChecks().size() == 1);
    CHECK(scripted.GrammarChecks()[0].grammar == R"(root ::= "yes")");
    CHECK(scripted.GrammarChecks()[0].text == "no");
    CHECK_FALSE(pSeat->Pending()); // None of them asks the seat.

    // The state calls reach the wrapped backend: FakeBackend's refusals come through.
    app::ModelSeatBackend fake(std::make_unique<test::FakeBackend>(4096), pSeat);
    CHECK(fake.ContextSize() == 4096);
    CHECK_THROWS_WITH(fake.SaveState(), ContainsSubstring("FakeBackend::SaveState"));
    CHECK_THROWS_WITH(fake.RestoreState(std::span<const std::uint8_t>()), ContainsSubstring("FakeBackend::RestoreState"));

    // It needs a backend and a seat.
    CHECK_THROWS_AS(app::ModelSeatBackend(nullptr, pSeat), core::ContractError);
    CHECK_THROWS_AS(app::ModelSeatBackend(MakeScripted(), nullptr), core::ContractError);
}

TEST_CASE("With the seat off, or the call's label in Model mode, Complete is the real model's", "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    inference::GenerationSpec spec;
    spec.maxTokens = 64;

    SECTION("the seat off")
    {
    }
    SECTION("the label in Model mode")
    {
        pSeat->SetEnabled(true);
        pSeat->SetMode(app::SeatLabel::Narrate, app::SeatMode::Model);
    }
    test::Streamed streamed;
    test::AsyncCall<inference::Completion> call(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, streamed.Callback(), stop); });
    const inference::Completion completion = call.Get();
    CHECK(completion.text == MODEL_REPLY);
    CHECK(scripted.Calls().size() == 1);
    CHECK(streamed.Pieces().size() > 1); // Streamed piece by piece, as the model streams.
    CHECK_FALSE(pSeat->Pending());
}

TEST_CASE("In Human mode, Complete waits for the owner's reply and returns it as the model's", "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    pSeat->SetEnabled(true); // Every label starts in Human mode.
    inference::GenerationSpec spec;
    spec.maxTokens = 64;
    test::Streamed streamed;
    test::AsyncCall<inference::Completion> call(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, streamed.Callback(), stop); });

    const app::SeatRequest request = test::WaitForRequest(*pSeat);
    CHECK(request.label == app::SeatLabel::Narrate);
    REQUIRE(request.prompt.messages.size() == 2);
    CHECK(request.prompt.messages[1].content == "I walk.");
    CHECK(request.prompt.stableCount == 1);
    CHECK(request.spec.maxTokens == 64);
    CHECK(request.spec.thinkingMaxTokens == 0);
    CHECK_FALSE(request.draft);
    CHECK(streamed.Pieces().empty()); // Nothing reaches the Play window before the reply.

    const std::string reply = "You walk to the door.";
    REQUIRE(pSeat->Submit(request.id, reply, "No thinking budget, so this is dropped."));
    const inference::Completion completion = call.Get();
    CHECK(completion.text == reply);
    CHECK_FALSE(completion.stoppedAtLimit);
    CHECK(completion.tokensGenerated == scripted.CountTokens(reply));
    CHECK(completion.thinking.empty());
    CHECK(completion.thinkingTokens == 0);
    CHECK(completion.promptTokens == scripted.CountPromptTokens(NARRATE_PROMPT));
    CHECK(completion.tokensEvaluated == completion.promptTokens);
    CHECK(completion.tokensReused == 0);
    CHECK(completion.promptMs == 0.0);
    CHECK(completion.thinkingMs == 0.0);
    CHECK_FALSE(completion.cancelled);
    CHECK(streamed.Pieces() == std::vector<std::string>{reply}); // Once, whole.
    CHECK(scripted.Calls().empty());                             // The real model was not asked.
    CHECK_FALSE(pSeat->Pending());
}

TEST_CASE("In Draft mode, the real model's answer is the draft, and nothing streams before the submit",
          "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    pSeat->SetEnabled(true);
    pSeat->SetMode(app::SeatLabel::Narrate, app::SeatMode::Draft);
    inference::GenerationSpec spec;
    spec.maxTokens = 64;
    spec.thinkingMaxTokens = 64;
    test::Streamed streamed;
    test::AsyncCall<inference::Completion> call(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, streamed.Callback(), stop); });

    const app::SeatRequest request = test::WaitForRequest(*pSeat);
    REQUIRE(request.draft);
    CHECK(request.draft->text == MODEL_REPLY);
    CHECK(request.draft->thinking == MODEL_THOUGHT);
    CHECK(scripted.Calls().size() == 1);
    CHECK(streamed.Pieces().empty());

    SECTION("submitted unchanged: the model's own completion, with its counts and timings")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // A wait the model's timings must not show.
        REQUIRE(pSeat->Submit(request.id, request.draft->text, request.draft->thinking));
        const inference::Completion completion = call.Get();
        CHECK(completion.text == MODEL_REPLY);
        CHECK(completion.thinking == MODEL_THOUGHT);
        CHECK(completion.tokensGenerated == request.draft->tokensGenerated);
        CHECK(completion.thinkingTokens == request.draft->thinkingTokens);
        CHECK(completion.promptTokens == request.draft->promptTokens);
        CHECK(completion.generateMs == request.draft->generateMs);
        CHECK(streamed.Pieces() == std::vector<std::string>{MODEL_REPLY});
    }
    SECTION("edited: the owner's reply")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // The owner's time is the answer's time.
        REQUIRE(pSeat->Submit(request.id, "The owner walks.", "Owner thoughts."));
        const inference::Completion completion = call.Get();
        CHECK(completion.text == "The owner walks.");
        CHECK(completion.tokensGenerated == scripted.CountTokens("The owner walks."));
        CHECK(completion.thinking == "Owner thoughts.");
        CHECK(completion.thinkingTokens == scripted.CountTokens("Owner thoughts."));
        CHECK(completion.generateMs >= 5.0);
        CHECK(streamed.Pieces() == std::vector<std::string>{"The owner walks."});
    }
    CHECK(scripted.Calls().size() == 1); // The draft was the only real call.
}

TEST_CASE("A Draft call stopped while the model drafts returns the model's cancelled completion at once",
          "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    pSeat->SetEnabled(true);
    pSeat->SetMode(app::SeatLabel::Narrate, app::SeatMode::Draft);
    inference::GenerationSpec spec;
    spec.maxTokens = 64;
    std::stop_source stop;
    stop.request_stop();

    const inference::Completion completion = backend.Complete(NARRATE_PROMPT, spec, {}, stop.get_token());
    CHECK(completion.cancelled);
    // The model's cancelled completion (it counted the prompt), not one the seat made after a wait.
    CHECK(completion.promptTokens == scripted.CountPromptTokens(NARRATE_PROMPT));
    CHECK(scripted.Calls().size() == 1);
    CHECK_FALSE(pSeat->Pending());
}

TEST_CASE("A reply over the call's limits is cut at whole tokens, as the model would have been stopped",
          "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    app::ModelSeatBackend backend(MakeScripted(), pSeat);
    pSeat->SetEnabled(true);

    // ScriptedBackend counts one token per 4 bytes, rounded up.
    inference::GenerationSpec spec;
    spec.maxTokens = 2;
    spec.thinkingMaxTokens = 1;
    test::AsyncCall<inference::Completion> call(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, {}, stop); });
    REQUIRE(pSeat->Submit(test::WaitForRequest(*pSeat).id, "abcdefghijkl", "abcdefgh"));
    const inference::Completion completion = call.Get();
    CHECK(completion.text == "abcdefgh");
    CHECK(completion.stoppedAtLimit);
    CHECK(completion.tokensGenerated == 2);
    CHECK(completion.thinking == "abcd");
    CHECK(completion.thinkingStoppedAtLimit);
    CHECK(completion.thinkingTokens == 1);

    // The cut never splits a UTF-8 character: "abc" plus the two bytes of e-acute would be 2 tokens.
    inference::GenerationSpec narrow;
    narrow.maxTokens = 1;
    test::AsyncCall<inference::Completion> second(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, narrow, {}, stop); });
    REQUIRE(pSeat->Submit(test::WaitForRequest(*pSeat).id, "abc\xC3\xA9xyz", ""));
    const inference::Completion utf8 = second.Get();
    CHECK(utf8.text == "abc");
    CHECK(utf8.stoppedAtLimit);
    CHECK(utf8.tokensGenerated == 1);
}

TEST_CASE("A stop or a Withdraw ends a call waiting at the seat as cancelled, and the request is gone",
          "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    std::unique_ptr<test::ScriptedBackend> pScripted = MakeScripted();
    test::ScriptedBackend& scripted = *pScripted;
    app::ModelSeatBackend backend(std::move(pScripted), pSeat);
    pSeat->SetEnabled(true);
    inference::GenerationSpec spec;
    spec.maxTokens = 64;
    test::Streamed streamed;
    test::AsyncCall<inference::Completion> call(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, streamed.Callback(), stop); });

    const app::SeatRequest request = test::WaitForRequest(*pSeat);
    SECTION("the stop token, as a Cancel requests it")
    {
        call.Stop();
    }
    SECTION("Withdraw, with no stop requested")
    {
        CHECK(pSeat->Withdraw(request.id));
    }
    const inference::Completion completion = call.Get();
    CHECK(completion.cancelled);
    CHECK(completion.text.empty());
    CHECK_FALSE(pSeat->Pending());
    CHECK(streamed.Pieces().empty());
    CHECK(scripted.Calls().empty());
    // The window's click that came too late: stale, so nothing happens and nothing throws.
    CHECK_FALSE(pSeat->Submit(request.id, "Too late.", ""));
}

TEST_CASE("Withdrawing a maintenance fold ends that maintenance cleanly, and the next turn folds again",
          "[model_seat]")
{
    // The session tests' folding budgets: with a context of 3000 and 2000-byte narrations, two turns fit the
    // transcript and three do not. So turns 1 to 3 need no fold, and the maintenance after turn 3 folds.
    core::Settings settings;
    settings.chatModel = "models/chat.gguf";
    settings.embeddingModel = "models/embed.gguf";
    settings.contextSize = 3000;
    settings.budget = {64, 600, 200, 400, 1024, 1024, 512, 100};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    // Resolve calls get MOVE, summary calls "Summary.", narration 2000 bytes.
    auto responder = [](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("intent") != std::string::npos) {
            return MOVE;
        }
        const std::string& last = prompt.messages.back().content;
        if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
            return "Summary.";
        }
        return std::string(2000, 'w');
    };
    auto pSeat = std::make_shared<app::ModelSeat>();
    app::ModelSeatBackend backend(std::make_unique<test::ScriptedBackend>(3000, responder), pSeat);
    turn::Worker worker(settings, backend);

    worker.Post(turn::StartRun{test::TestDataDir() / "worlds" / "mini",
                               test::FreshTempDir("seat_withdraw_maintenance") / "run.db", 7});
    REQUIRE(std::holds_alternative<turn::RunOpened>(UntilClosing(worker).back()));
    for (int played = 1; played <= 2; ++played) {
        worker.Post(turn::SubmitInput{"I walk."});
        REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));
    }

    // From turn 3 on, the owner answers summaries; everything else goes to the model.
    test::SeatOnlyFor(*pSeat, app::SeatLabel::Summarize, app::SeatMode::Human);
    worker.Post(turn::SubmitInput{"I walk."});
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(UntilClosing(worker).back()));

    // The maintenance after turn 3 asks for a fold; Withdraw releases it.
    const app::SeatRequest fold = test::WaitForRequest(*pSeat);
    CHECK(fold.label == app::SeatLabel::Summarize);
    CHECK(pSeat->Withdraw(fold.id));
    CHECK_FALSE(pSeat->Pending());

    // The next turn proceeds. The withdrawn maintenance reported nothing (no MemoryUpdated, no MaintenanceFailed)
    // and stored nothing, so the transcript is still over budget and the turn's own fit loop asks for the fold
    // again.
    worker.Post(turn::SubmitInput{"I walk on."});
    const app::SeatRequest again = test::WaitForRequest(*pSeat);
    CHECK(again.label == app::SeatLabel::Summarize);
    CHECK(again.id == fold.id + 1);
    pSeat->SetEnabled(false); // Any later fold or condense goes to the model.
    REQUIRE(pSeat->Submit(again.id, "The first turn, summed up.", ""));
    const std::vector<turn::Event> events = UntilClosing(worker);
    for (const turn::Event& event : events) {
        CHECK_FALSE(std::holds_alternative<turn::MemoryUpdated>(event));
        CHECK_FALSE(std::holds_alternative<turn::MaintenanceFailed>(event));
    }
    REQUIRE(std::holds_alternative<turn::TurnCompleted>(events.back()));
    const turn::TurnCompleted& completed = std::get<turn::TurnCompleted>(events.back());
    CHECK(completed.record.number == 4);
    REQUIRE_FALSE(completed.memory.summaries.empty());
    CHECK(completed.memory.summaries.front().fromTurn == 1);
    CHECK(completed.memory.summaries.front().text == "The first turn, summed up.");
}

TEST_CASE("Turning the seat off while a call waits leaves that call waiting; later calls go to the model",
          "[model_seat]")
{
    auto pSeat = std::make_shared<app::ModelSeat>();
    app::ModelSeatBackend backend(MakeScripted(), pSeat);
    pSeat->SetEnabled(true);
    inference::GenerationSpec spec;
    spec.maxTokens = 64;
    test::AsyncCall<inference::Completion> first(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, {}, stop); });

    const app::SeatRequest request = test::WaitForRequest(*pSeat);
    pSeat->SetEnabled(false);
    CHECK(pSeat->Pending());
    REQUIRE(pSeat->Submit(request.id, "You walk.", ""));
    CHECK(first.Get().text == "You walk.");

    test::AsyncCall<inference::Completion> second(
        [&](std::stop_token stop) { return backend.Complete(NARRATE_PROMPT, spec, {}, stop); });
    CHECK(second.Get().text == MODEL_REPLY);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'app/model/ModelSeatBackend.h'`.

- [ ] **Step 3: Write the header**

Create `src/app/model/ModelSeatBackend.h`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "app/model/ModelSeat.h"
#include "inference/Llm.h"

namespace morpheus::app {

// A backend through which the model seat answers the engine's model calls (the model seat design). Everything but
// Complete is forwarded to the real backend unchanged. Complete forwards too while the seat is off or the call's
// label is in Model mode; otherwise it publishes the call to the seat, waits for the owner's reply (in Draft mode
// after the real model has answered, as the draft), and returns the reply as the model's completion. Used on the
// engine worker thread like any backend; the seat is shared with the UI thread.
class ModelSeatBackend final : public inference::LlmBackend {
private:
    std::unique_ptr<inference::LlmBackend> _pInner = nullptr; // The real backend.
    std::shared_ptr<ModelSeat> _pSeat = nullptr;

public:
    // Throws ContractError when `pInner` or `pSeat` is null.
    ModelSeatBackend(std::unique_ptr<inference::LlmBackend> pInner, std::shared_ptr<ModelSeat> pSeat);

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    // A reply typed by the owner: its text is cut to spec.maxTokens and its thinking to spec.thinkingMaxTokens (and
    // dropped when that is 0), each at the longest prefix within the limit by the real tokenizer that ends on a UTF-8
    // character; stoppedAtLimit and thinkingStoppedAtLimit say whether they were cut. Its token counts are the real
    // counts of what is kept, the prompt counts as evaluated in full (none reused), and generateMs is the time the
    // request waited. A draft submitted unchanged is the model's own completion. The answer reaches onToken once,
    // whole. A stop or a Withdraw while waiting returns a completion with only `cancelled` set, which the caller
    // treats as any cancelled model call (a turn closes with TurnCancelled, a fold or condense stores nothing).
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::string CheckGrammar(const std::string& grammar, std::string_view text) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
};

} // namespace morpheus::app
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/ModelSeatBackend.cpp`:

```cpp
#include "app/model/ModelSeatBackend.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include "core/Errors.h"

namespace morpheus::app {

ModelSeatBackend::ModelSeatBackend(std::unique_ptr<inference::LlmBackend> pInner, std::shared_ptr<ModelSeat> pSeat)
    : _pInner(std::move(pInner))
    , _pSeat(std::move(pSeat))
{
    MORPHEUS_REQUIRE(_pInner != nullptr, "ModelSeatBackend: no backend to wrap (the backend factory returned none)");
    MORPHEUS_REQUIRE(_pSeat != nullptr, "ModelSeatBackend: no model seat");
}

int ModelSeatBackend::CountTokens(std::string_view text)
{
    return _pInner->CountTokens(text);
}

int ModelSeatBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pInner->CountPromptTokens(prompt);
}

int ModelSeatBackend::ContextSize() const
{
    return _pInner->ContextSize();
}

inference::Completion ModelSeatBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                                 const inference::TokenCallback& onToken, std::stop_token stop)
{
    // 1. With the seat off, or this kind of call in Model mode, the real model answers, as without the seat.
    if (!_pSeat->Enabled()) {
        return _pInner->Complete(prompt, spec, onToken, stop);
    }
    const SeatLabel label = SeatLabelOf(prompt, spec);
    const SeatMode mode = _pSeat->ModeOf(label);
    if (mode == SeatMode::Model) {
        return _pInner->Complete(prompt, spec, onToken, stop);
    }
    // The seat answers: the interface's contract holds for it too.
    MORPHEUS_REQUIRE(spec.maxTokens > 0, "Complete: maxTokens must be positive");
    MORPHEUS_REQUIRE(spec.thinkingMaxTokens >= 0, "Complete: thinkingMaxTokens must not be negative");

    // 2. Draft: the real model answers first, with no token callback, so nothing reaches the Play window until the
    // owner submits. A draft cancelled midway ends the call as a cancelled model call.
    std::optional<inference::Completion> draft = {};
    if (mode == SeatMode::Draft) {
        draft = _pInner->Complete(prompt, spec, {}, stop);
        if (draft->cancelled) {
            return std::move(*draft);
        }
    }

    // 3. Publish the request and wait for the owner's reply, or for the stop token.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point published = Clock::now();
    const std::optional<SeatReply> reply =
        _pSeat->Await(SeatRequest{.label = label, .prompt = prompt, .spec = spec, .draft = draft}, *_pInner, stop);
    if (!reply) {
        // Stopped or withdrawn: as a model call cancelled midway, which the caller discards.
        inference::Completion cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
    const double waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - published).count();

    // 4. The completion. Thinking counts only when the call has a thinking budget.
    const bool thinks = spec.thinkingMaxTokens > 0;
    inference::Completion completion;
    if (draft && reply->text == draft->text && (!thinks || reply->thinking == draft->thinking)) {
        // The draft submitted unchanged: the model's own completion, with its token counts and timings.
        completion = std::move(*draft);
    } else {
        // The longest prefix of `text` within `maxTokens` tokens of the real tokenizer that ends on a UTF-8
        // character boundary; `cut` says whether anything was dropped.
        auto keepWithin = [this](const std::string& text, int maxTokens, bool& cut) -> std::string {
            cut = _pInner->CountTokens(text) > maxTokens;
            if (!cut) {
                return text;
            }
            // The places a cut may fall: the start, each byte that begins a UTF-8 character, and the end.
            std::vector<std::size_t> bounds = {0};
            for (std::size_t at = 1; at < text.size(); ++at) {
                if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
                    bounds.push_back(at);
                }
            }
            bounds.push_back(text.size());
            // Bisect: the prefix ending at bounds[low] always fits (low starts at the empty prefix), the one ending
            // at bounds[high] never does (high starts at the whole text).
            std::size_t low = 0;
            std::size_t high = bounds.size() - 1;
            while (high - low > 1) {
                const std::size_t middle = low + (high - low) / 2;
                if (_pInner->CountTokens(std::string_view(text).substr(0, bounds[middle])) <= maxTokens) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return text.substr(0, bounds[low]);
        };
        completion.text = keepWithin(reply->text, spec.maxTokens, completion.stoppedAtLimit);
        completion.tokensGenerated = _pInner->CountTokens(completion.text);
        if (thinks) {
            completion.thinking = keepWithin(reply->thinking, spec.thinkingMaxTokens, completion.thinkingStoppedAtLimit);
            completion.thinkingTokens = _pInner->CountTokens(completion.thinking);
        }
        // The whole prompt counts as evaluated; generating took the time the request waited.
        completion.promptTokens = _pInner->CountPromptTokens(prompt);
        completion.tokensEvaluated = completion.promptTokens;
        completion.tokensReused = 0;
        completion.generateMs = waitedMs;
    }

    // 5. The answer reaches the Play window in one piece, as if it had streamed.
    if (onToken && !completion.text.empty()) {
        onToken(completion.text);
    }
    return completion;
}

std::string ModelSeatBackend::CheckGrammar(const std::string& grammar, std::string_view text)
{
    return _pInner->CheckGrammar(grammar, text);
}

std::vector<std::vector<float>> ModelSeatBackend::Embed(const std::vector<std::string>& texts,
                                                        inference::EmbedPurpose purpose)
{
    return _pInner->Embed(texts, purpose);
}

std::string ModelSeatBackend::EmbeddingModelId() const
{
    return _pInner->EmbeddingModelId();
}

std::vector<std::uint8_t> ModelSeatBackend::SaveState()
{
    return _pInner->SaveState();
}

void ModelSeatBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pInner->RestoreState(state);
}

} // namespace morpheus::app
```

- [ ] **Step 5: Add the source to the build**

In `src/app/CMakeLists.txt`, replace

```cmake
    model/ModelSeat.cpp
```

with

```cmake
    model/ModelSeat.cpp
    model/ModelSeatBackend.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[model_seat]"`
Expected: all 17 `[model_seat]` test cases pass.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 303`.

- [ ] **Step 7: Commit**

```bash
git add src/app/model/ModelSeatBackend.h src/app/model/ModelSeatBackend.cpp src/app/CMakeLists.txt tests/app/ModelSeatBackendTests.cpp
git commit -m "Add ModelSeatBackend, which lets the model seat answer model calls" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The app answers through the seat

`AppController` owns the seat and wraps every backend its factory makes, so the engine host's backends (rebuilt ones too) answer through the same seat. With the seat off the wrapper is transparent, so every existing app test still passes.

**Files:**
- Modify: `src/app/model/AppController.h`, `src/app/model/AppController.cpp`
- Test: `tests/app/AppControllerTests.cpp`

**Interfaces:**
- Consumes: `ModelSeat`, `SeatLabel`, `SeatMode`, `SeatRequest` (Task 2); `ModelSeatBackend` (Task 3); `test::SeatOnlyFor` (Task 2).
- Produces: `ModelSeat& AppController::Seat();` Every backend `EngineHost` holds is `ModelSeatBackend(factory(settings), seat)`.

- [ ] **Step 1: Write the failing tests**

In `tests/app/AppControllerTests.cpp`, replace

```cpp
#include "app/AppTestSupport.h"
#include "app/model/AppController.h"
```

with

```cpp
#include "app/AppTestSupport.h"
#include "app/SeatTestSupport.h"
#include "app/model/AppController.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "core/TurnRecord.h"
```

and append:

```cpp
TEST_CASE("A resolve reply typed at the model seat that is not JSON proposes nothing, as a model mistake does",
          "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_resolve");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You stay where you are.")));
    OpenNewRun(controller, "seat");
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    const app::SeatRequest request = *controller.Seat().Pending();
    CHECK(request.label == app::SeatLabel::Resolve);
    CHECK_FALSE(request.spec.grammar.empty());
    CHECK_FALSE(request.draft);
    REQUIRE(controller.Seat().Submit(request.id, "I would rather not.", ""));
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // The engine reads the reply as it reads a model's: unreadable, so nothing is proposed, and the turn goes on.
    REQUIRE(controller.Run().History().size() == 1);
    const core::TurnRecord& turn = controller.Run().History()[0];
    CHECK(turn.resolve.at("unparsed") == "I would rather not.");
    CHECK(turn.applied.empty());
    CHECK_FALSE(turn.timings.at("resolve").value("error", std::string()).empty());
    CHECK(turn.narration == "You stay where you are.");
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.Run().State().at("player").at("fields").at("location") == "shore");
}

TEST_CASE("Cancelling a turn while the model seat waits closes it with TurnCancelled", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_cancel");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk.")));
    OpenNewRun(controller, "seat");
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk slowly.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // Cancelled, not failed: no turn, no error, and the input comes back.
    CHECK(controller.Run().History().empty());
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.TakeRestoredInput() == std::optional<std::string>("I walk slowly."));
    CHECK_FALSE(controller.Seat().Pending());
}

TEST_CASE("The model seat keeps its switch and modes across a restart that reloads the models", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_restart");
    std::atomic<int> made = 0;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder("You walk."));
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        ++made;
        return scripted(settings);
    });
    OpenNewRun(controller, "restarted");
    app::ModelSeat& seat = controller.Seat();
    test::SeatOnlyFor(seat, app::SeatLabel::Resolve, app::SeatMode::Human);
    seat.SetMode(app::SeatLabel::Narrate, app::SeatMode::Draft);

    // A GPU layers change reloads the models.
    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.gpuLayers = edit.gpuLayers == 7 ? 8 : 7;
    REQUIRE(controller.ApplySettings(edit).empty());
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    CHECK(made == 2);

    CHECK(&controller.Seat() == &seat);
    CHECK(seat.Enabled());
    CHECK(seat.ModeOf(app::SeatLabel::Resolve) == app::SeatMode::Human);
    CHECK(seat.ModeOf(app::SeatLabel::Narrate) == app::SeatMode::Draft);
    CHECK(seat.ModeOf(app::SeatLabel::Review) == app::SeatMode::Model);

    // The reloaded backend answers through the same seat.
    controller.Submit("I walk.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    CHECK(controller.Seat().Pending()->label == app::SeatLabel::Resolve);
    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `'Seat': is not a member of 'morpheus::app::AppController'`.

- [ ] **Step 3: Own the seat and wrap the factory**

In `src/app/model/AppController.h`, replace

```cpp
#include <filesystem>
#include <optional>
```

with

```cpp
#include <filesystem>
#include <memory>
#include <optional>
```

replace

```cpp
#include "app/model/EngineHost.h"
```

with

```cpp
#include "app/model/EngineHost.h"
#include "app/model/ModelSeat.h"
```

replace

```cpp
// The app apart from drawing (UI thread only): the worlds and their runs, the settings, the engine and the open run.
```

with

```cpp
// The app apart from drawing (UI thread only): the worlds and their runs, the settings, the engine, the open run and
// the model seat.
```

replace

```cpp
    AppPaths _paths = {};
    EngineHost _engine;
```

with

```cpp
    AppPaths _paths = {};
    // The model seat, shared with every backend the engine host builds. Declared before _engine, whose factory wraps
    // each backend around it: it exists first, and the backends' shares keep it alive as long as they live.
    std::shared_ptr<ModelSeat> _pSeat = std::make_shared<ModelSeat>();
    EngineHost _engine;
```

replace

```cpp
    // Loads the settings (DataError when they are invalid: the app cannot run without them), lists the worlds
    // (DataError when the worlds folder is missing) and starts loading the engine.
    AppController(AppPaths paths, EngineHost::BackendFactory factory);
```

with

```cpp
    // Loads the settings (DataError when they are invalid: the app cannot run without them), lists the worlds
    // (DataError when the worlds folder is missing) and starts loading the engine. Every backend `factory` makes is
    // wrapped in a ModelSeatBackend around the seat; with the seat off, the wrapper only forwards.
    AppController(AppPaths paths, EngineHost::BackendFactory factory);
```

and replace

```cpp
    const std::vector<WorldEntry>& Worlds() const;
```

with

```cpp
    const std::vector<WorldEntry>& Worlds() const;
    // The model seat: its switch, the modes, and the call waiting for a reply.
    ModelSeat& Seat();
```

In `src/app/model/AppController.cpp`, replace

```cpp
#include "core/Errors.h"
```

with

```cpp
#include "app/model/ModelSeatBackend.h"
#include "core/Errors.h"
```

replace

```cpp
AppController::AppController(AppPaths paths, EngineHost::BackendFactory factory)
    : _paths(std::move(paths))
    , _engine(std::move(factory))
{
```

with

```cpp
AppController::AppController(AppPaths paths, EngineHost::BackendFactory factory)
    : _paths(std::move(paths))
    , _engine([pSeat = _pSeat, factory = std::move(factory)](
                  const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        // Every backend answers through the model seat, a rebuilt one too, so the switch and modes survive restarts.
        return std::make_unique<ModelSeatBackend>(factory(settings), pSeat);
    })
{
```

and replace

```cpp
const std::vector<WorldEntry>& AppController::Worlds() const
{
    return _worlds;
}
```

with

```cpp
const std::vector<WorldEntry>& AppController::Worlds() const
{
    return _worlds;
}

ModelSeat& AppController::Seat()
{
    return *_pSeat;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[app_controller],[engine_host],[ui]"`
Expected: all pass (the existing ones unchanged: the seat is off).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 306`.

- [ ] **Step 5: Commit**

```bash
git add src/app/model/AppController.h src/app/model/AppController.cpp tests/app/AppControllerTests.cpp
git commit -m "Let the app answer model calls through the model seat" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: The Model seat window

The window shows the switch and the modes always, and a waiting call with its limits, prompt, grammar and draft, a reply and a thinking box, and Check, Submit and Withdraw. Withdraw releases any pending request: while a turn or regenerate runs it cancels the command (the Play window's Cancel), otherwise (between-turn maintenance) it calls `ModelSeat::Withdraw`. There is no separate Cancel turn button. The Context window's prompt drawing becomes `DrawPrompt`, which both windows call. The status bar says when the engine waits for the seat.

**Files:**
- Create: `src/app/ui/ModelSeatWindow.cpp`
- Modify: `src/app/ui/Windows.h`, `src/app/ui/Ui.h`, `src/app/ui/Ui.cpp`, `src/app/ui/ContextWindow.cpp`, `src/app/CMakeLists.txt`
- Test: `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: `AppController::Seat()`, `AppController::Cancel()` (Task 4, existing); `ModelSeat` (`Submit(id, ...)`, `Check(id, ...)`, `Withdraw(id)`), `SEAT_LABELS`, `SeatLabelName`, `SeatRequest`, `SeatCheck` (Task 2); `test::SeatOnlyFor` (Task 2); `test::ScriptedBackend::GrammarChecks` (Task 1).
- Produces:
  - `constexpr const char* MODEL_SEAT_WINDOW = "Model seat";`
  - `void DrawPrompt(const inference::Prompt& prompt, bool& showRaw);` (in `Ui.cpp`, declared in `Windows.h`).
  - `class ModelSeatWindow { public: void Draw(AppController& controller); };` with items labelled exactly `Answer as the model` (checkbox), `Check`, `Submit`, `Withdraw`, drawn directly in the window (the tests find them by those IDs).

- [ ] **Step 1: Write the failing tests**

In `tests/app/UiTests.cpp`, replace

```cpp
#include <algorithm>
#include <chrono>
```

with

```cpp
#include <algorithm>
#include <atomic>
#include <chrono>
```

replace

```cpp
#include <string>
#include <vector>

#include "SceneWorld.h"
#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/model/AppController.h"
#include "app/model/Inspect.h"
```

with

```cpp
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName and ActivateItemByID: pressing the window's buttons.
#include <nlohmann/json.hpp>

#include "SceneWorld.h"
#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/SeatTestSupport.h"
#include "app/model/AppController.h"
#include "app/model/Inspect.h"
#include "app/model/ModelSeat.h"
```

replace

```cpp
#include "app/ui/Ui.h"
```

with

```cpp
#include "app/ui/Ui.h"
#include "app/ui/Windows.h"
#include "fakes/ScriptedBackend.h"
```

inside the anonymous namespace, after `DrawFrames`, add:

```cpp
// Draws the Model seat window alone, at a place and size that show its buttons, for `frames` frames, taking the
// worker's events before each as the app does. (Undocked: no Ui is drawn in these tests' contexts.)
void DrawSeatWindow(test::HeadlessImGui& imgui, app::AppController& controller, app::ModelSeatWindow& window,
                    int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        controller.Frame();
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1200.0f, 880.0f));
            window.Draw(controller);
        });
    }
}

// Presses the Model seat window's item `label` on the next frame, as a click would: ImGui's programmatic activation.
// Call between frames.
void PressSeatItem(const char* label)
{
    ImGuiWindow* pWindow = ImGui::FindWindowByName(app::MODEL_SEAT_WINDOW);
    REQUIRE(pWindow != nullptr);
    ImGui::ActivateItemByID(pWindow->GetID(label));
}
```

and append:

```cpp
TEST_CASE("The Model seat window draws with no call waiting, and its checkbox turns the seat on", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_idle");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;

    DrawSeatWindow(imgui, controller, window, 3); // Models loading, the seat off.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawSeatWindow(imgui, controller, window, 2);
    CHECK_FALSE(controller.Seat().Enabled());

    PressSeatItem("Answer as the model");
    DrawSeatWindow(imgui, controller, window, 2); // On, nothing waits.
    CHECK(controller.Seat().Enabled());
    CHECK_FALSE(controller.Seat().Pending());
}

TEST_CASE("Submit in the Model seat window answers a Human call with the reply box's text", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_human");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawSeatWindow(imgui, controller, window, 2); // The call, its limits and grammar, empty boxes, the prompt.
    CHECK(controller.Seat().Pending());           // Drawing answers nothing.

    PressSeatItem("Submit");
    DrawSeatWindow(imgui, controller, window, 1);
    CHECK_FALSE(controller.Seat().Pending());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // The empty reply box reached the engine as the model's resolve output: unreadable, nothing proposed.
    REQUIRE(controller.Run().History().size() == 1);
    CHECK(controller.Run().History()[0].resolve.at("unparsed") == "");
    CHECK(controller.Run().History()[0].applied.empty());
    CHECK(controller.Run().History()[0].narration == NARRATION);
}

TEST_CASE("The Model seat window shows a Draft call's draft, checks it and submits it", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_draft");
    // The factory hands out the ScriptedBackend it made, to see the grammar checks reach it.
    std::atomic<test::ScriptedBackend*> pMade = nullptr;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder());
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        std::unique_ptr<inference::LlmBackend> pBackend = scripted(settings);
        pMade = static_cast<test::ScriptedBackend*>(pBackend.get());
        return pBackend;
    });
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Draft);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    const app::SeatRequest request = *controller.Seat().Pending();
    REQUIRE(request.draft);
    CHECK(request.draft->text == MOVE);
    DrawSeatWindow(imgui, controller, window, 2); // The draft's thinking, the grammar, the prefilled boxes.

    // Check: the reply's token count and the grammar's verdict, from the real backend. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    REQUIRE(pMade.load() != nullptr);
    REQUIRE(pMade.load()->GrammarChecks().size() == 1);
    CHECK(pMade.load()->GrammarChecks()[0].text == MOVE);
    CHECK(controller.Seat().Pending());

    // Submit sends the draft unchanged.
    PressSeatItem("Submit");
    DrawSeatWindow(imgui, controller, window, 1);
    CHECK_FALSE(controller.Seat().Pending());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    REQUIRE(controller.Run().History().size() == 1);
    CHECK(controller.Run().History()[0].resolve == nlohmann::json::parse(MOVE));
    CHECK(controller.Run().State().at("player").at("fields").at("location") == "lighthouse");
}

TEST_CASE("The UI draws the status bar and the docked windows while the model seat waits", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_status");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::Ui ui;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawFrames(imgui, controller, ui); // "Waiting for the model seat: the resolve call"; the seat window docked.
    CHECK(controller.Seat().Pending());

    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawFrames(imgui, controller, ui);
}

TEST_CASE("Withdraw in the Model seat window cancels the turn whose call waits", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_withdraw");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawSeatWindow(imgui, controller, window, 2);

    // A turn runs, so Withdraw cancels it as the Play window's Cancel does: the wait wakes on the stop token.
    PressSeatItem("Withdraw");
    DrawSeatWindow(imgui, controller, window, 1);
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawSeatWindow(imgui, controller, window, 1); // No call waits any more.

    // Closed with TurnCancelled: no turn, no error, and the input comes back.
    CHECK(controller.Run().History().empty());
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.TakeRestoredInput() == std::optional<std::string>("I walk to the lighthouse."));
    CHECK_FALSE(controller.Seat().Pending());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/app/UiTests.cpp`: `'ModelSeatWindow': is not a member of 'morpheus::app'` and `'MODEL_SEAT_WINDOW': is not a member of 'morpheus::app'`.

- [ ] **Step 3: Declare the window and the shared prompt drawing**

In `src/app/ui/Windows.h`, replace

```cpp
#include <cstddef>
#include <optional>
```

with

```cpp
#include <cstddef>
#include <cstdint>
#include <optional>
```

replace

```cpp
#include "app/model/Catalog.h"
#include "app/model/SettingsEdit.h"
```

with

```cpp
#include "app/model/Catalog.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "inference/Llm.h"
```

replace

```cpp
constexpr const char* SETTINGS_WINDOW = "Settings";
```

with

```cpp
constexpr const char* SETTINGS_WINDOW = "Settings";
constexpr const char* MODEL_SEAT_WINDOW = "Model seat";
```

replace

```cpp
// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);
```

with

```cpp
// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);

// A prompt as the Context window shows it, in the rest of the window: Sections / Raw radio buttons, then one
// collapsing section per message (titled by its role and layer) or the whole text, read-only. The choice is kept in
// `showRaw`. Used by the Context and Model seat windows.
void DrawPrompt(const inference::Prompt& prompt, bool& showRaw);
```

and append, after the `SettingsWindow` class (before the namespace's closing brace):

```cpp

// Debug: the owner answers the engine's model calls by hand (the model seat). Always: the "Answer as the model"
// switch and each label's Human / Draft / Model mode. While a call waits: its label and limits, the model's thinking
// (Draft mode) and the grammar, the reply and thinking boxes, Check, Submit and Withdraw, and the prompt as the
// Context window draws it. Withdraw releases the call: a running turn's or regenerate's by cancelling the command,
// a between-turn fold's or condense's at the seat. The texts typed stay until Submit and are reset when a new call
// arrives.
class ModelSeatWindow {
private:
    std::uint64_t _requestId = 0;         // The request _reply and _thinking belong to (SeatRequest::id); 0: none yet.
    std::string _reply = {};
    std::string _thinking = {};
    std::optional<SeatCheck> _check = {}; // The last Check of the reply as it is now; cleared when it is edited.
    bool _showRaw = false;                // The prompt as raw text instead of sections.

public:
    void Draw(AppController& controller);
};
```

- [ ] **Step 4: DrawPrompt, the status bar, the dock place and the draw call**

Replace the whole of `src/app/ui/Ui.cpp` with:

```cpp
#include "app/ui/Ui.h"

#include <cfloat>
#include <format>
#include <optional>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (the default layout) and BeginViewportSideBar (the status bar).
#include <imgui_stdlib.h>

#include "app/model/Inspect.h"
#include "app/model/ModelSeat.h"

namespace morpheus::app {

void TextWrappedColored(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawPrompt(const inference::Prompt& prompt, bool& showRaw)
{
    // The view: one section per message, or the whole text to copy.
    if (ImGui::RadioButton("Sections", !showRaw)) {
        showRaw = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Raw", showRaw)) {
        showRaw = true;
    }
    if (showRaw) {
        std::string text = RawPrompt(prompt);
        ImGui::InputTextMultiline("##raw", &text, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    } else {
        if (ImGui::BeginChild("sections")) {
            for (const PromptSection& section : PromptSections(prompt)) {
                if (ImGui::CollapsingHeader(section.title.c_str())) {
                    ImGui::TextWrapped("%s", section.text.c_str());
                }
            }
        }
        ImGui::EndChild();
    }
}

void Ui::Draw(AppController& controller)
{
    ImGuiViewport* pViewport = ImGui::GetMainViewport();

    // The status bar along the bottom: what the engine and the run are doing. It is drawn before the dock space,
    // which then fills the rest of the viewport.
    std::string status;
    switch (controller.Engine().CurrentStatus()) {
    case EngineHost::Status::Stopped:
        status = "Engine stopped";
        break;
    case EngineHost::Status::Loading:
        status = "Loading models...";
        break;
    case EngineHost::Status::Failed:
        status = "Engine failed: " + controller.Engine().Failure() + " (fix it in Settings and apply)";
        break;
    case EngineHost::Status::Ready:
        // A call waiting at the model seat holds the engine until it is answered or cancelled.
        if (const std::optional<SeatRequest> request = controller.Seat().Pending()) {
            status = std::format("Waiting for the model seat: the {} call", SeatLabelName(request->label));
            break;
        }
        switch (controller.Run().CurrentBusy()) {
        case RunModel::Busy::None:
            status = controller.Run().Open() ? std::format("Ready - {}, {} turns", controller.Run().Meta().worldId,
                                                           controller.Run().History().size())
                                             : std::string("Ready - no run open");
            break;
        case RunModel::Busy::Opening:
            status = "Opening the run...";
            break;
        case RunModel::Busy::Turn:
            status = "Playing the turn...";
            break;
        case RunModel::Busy::Regenerate:
            status = "Playing the turn again...";
            break;
        case RunModel::Busy::Undo:
            status = "Undoing the turn...";
            break;
        }
        break;
    }
    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", pViewport, ImGuiDir_Down, ImGui::GetFrameHeight(), barFlags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted(status.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    // The dock space. Without a saved layout (first launch, or imgui.ini deleted), the windows are docked into the
    // default one: runs and settings on the left, the story in the middle, state, memory and the model seat on the
    // right, and the turn log and context below the story.
    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, pViewport->WorkSize);
        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGui::DockBuilderDockWindow(RUNS_WINDOW, left);
        ImGui::DockBuilderDockWindow(SETTINGS_WINDOW, left);
        ImGui::DockBuilderDockWindow(PLAY_WINDOW, center);
        ImGui::DockBuilderDockWindow(STATE_WINDOW, right);
        ImGui::DockBuilderDockWindow(MEMORY_WINDOW, right);
        ImGui::DockBuilderDockWindow(MODEL_SEAT_WINDOW, right);
        ImGui::DockBuilderDockWindow(TURN_LOG_WINDOW, bottom);
        ImGui::DockBuilderDockWindow(CONTEXT_WINDOW, bottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, pViewport);

    _runs.Draw(controller);
    _play.Draw(controller);
    _state.Draw(controller);
    _turnLog.Draw(controller);
    _context.Draw(controller);
    _memory.Draw(controller);
    _settings.Draw(controller);
    _modelSeat.Draw(controller);
}

} // namespace morpheus::app
```

In `src/app/ui/Ui.h`, replace

```cpp
    SettingsWindow _settings = {};
```

with

```cpp
    SettingsWindow _settings = {};
    ModelSeatWindow _modelSeat = {};
```

- [ ] **Step 5: The Context window calls DrawPrompt**

Replace the whole of `src/app/ui/ContextWindow.cpp` with:

```cpp
#include "app/ui/Windows.h"

#include <optional>
#include <utility>

#include <imgui.h>

#include "app/model/Inspect.h"

namespace morpheus::app {

namespace {

constexpr const char* CALLS[] = {"lookup", "resolve", "narrate"}; // Model calls, in turn order.

} // namespace

void ContextWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(CONTEXT_WINDOW)) {
        const RunModel& run = controller.Run();
        const std::optional<context::Assembly>& assembly = run.LastAssembly();
        const core::TurnRecord* pLatest = run.LatestTurn();
        if (!assembly || !pLatest) {
            ImGui::TextDisabled("No prompt yet: it shows once a turn is played in this session.");
        } else {
            ImGui::Text("The resolve prompt of turn %d", pLatest->number);

            // Tokens per layer. Layer counts are raw text; the total is the whole templated prompt.
            const context::LayerTokens& tokens = assembly->tokens;
            const std::pair<const char*, int> layers[] = {
                {"L0 engine rules", tokens.engineRules}, {"L1 world", tokens.world},
                {"L2 summary", tokens.summary},          {"L3 transcript", tokens.transcript},
                {"Tail (T1-T4)", tokens.tail},           {"Total (templated)", tokens.total}};
            if (ImGui::BeginTable("layers", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Layer");
                ImGui::TableSetupColumn("Tokens");
                ImGui::TableHeadersRow();
                for (const auto& [name, count] : layers) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", count);
                }
                ImGui::EndTable();
            }
            ImGui::Text("Transcript: %d of %d budget tokens", assembly->transcriptCost, assembly->transcriptBudget);
            if (!assembly->Fits()) {
                TextWrappedColored(WARNING_COLOR, "This prompt was over budget.");
            }

            // KV-cache reuse per call of the latest turn.
            if (ImGui::BeginTable("cache", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                for (const char* header : {"Call", "Prompt", "Reused", "Evaluated"}) {
                    ImGui::TableSetupColumn(header);
                }
                ImGui::TableHeadersRow();
                for (const char* call : CALLS) {
                    const std::optional<CallStats> stats = CallStatsOf(pLatest->timings, call);
                    if (!stats) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(call);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->promptTokens);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->reused);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->evaluated);
                }
                ImGui::EndTable();
            }

            // The prompt itself, sections or raw, in the rest of the window.
            ImGui::SeparatorText("Prompt");
            DrawPrompt(assembly->prompt, showRaw);
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 6: Write the window**

Create `src/app/ui/ModelSeatWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/model/ModelSeat.h"

namespace morpheus::app {

namespace {

constexpr float REPLY_LINES = 8.0f;    // Height of the reply box, in lines of text.
constexpr float THINKING_LINES = 4.0f; // Height of the thinking box, in lines of text.

} // namespace

void ModelSeatWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(MODEL_SEAT_WINDOW)) {
        ModelSeat& seat = controller.Seat();

        // The switch: while it is off, the model answers every call as usual.
        bool enabled = seat.Enabled();
        if (ImGui::Checkbox("Answer as the model", &enabled)) {
            seat.SetEnabled(enabled);
        }

        // Who answers each kind of call: the owner (Human), the owner from the model's draft (Draft), or the model.
        if (ImGui::BeginTable("modes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* header : {"Call", "Human", "Draft", "Model"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (const SeatLabel label : SEAT_LABELS) {
                ImGui::PushID(static_cast<int>(label));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(SeatLabelName(label).c_str());
                for (const SeatMode mode : {SeatMode::Human, SeatMode::Draft, SeatMode::Model}) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(mode));
                    if (ImGui::RadioButton("##mode", seat.ModeOf(label) == mode)) {
                        seat.SetMode(label, mode);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        const std::optional<SeatRequest> request = seat.Pending();
        if (!request) {
            ImGui::TextDisabled("%s", seat.Enabled() ? "No call waits for a reply." : "Off: the model answers every call.");
        } else {
            // A new call: the reply starts as the model's draft (Draft mode) or empty (Human mode).
            if (request->id != _requestId) {
                _requestId = request->id;
                _reply = request->draft ? request->draft->text : std::string();
                _thinking = request->draft ? request->draft->thinking : std::string();
                _check.reset();
            }
            const inference::GenerationSpec& spec = request->spec;

            // Which call waits, and the limits the model would be held to.
            ImGui::SeparatorText(std::format("The {} call waits for a reply", SeatLabelName(request->label)).c_str());
            if (spec.thinkingMaxTokens > 0) {
                ImGui::Text("At most %d answer tokens, after at most %d thinking tokens", spec.maxTokens,
                            spec.thinkingMaxTokens);
            } else {
                ImGui::Text("At most %d answer tokens", spec.maxTokens);
            }
            if (!seat.Enabled()) {
                TextWrappedColored(WARNING_COLOR, "The seat is off: this call still waits for a reply; later calls go "
                                                  "to the model.");
            }

            // The model's thinking (Draft mode, read-only) and the grammar, folded away until opened.
            if (request->draft && !request->draft->thinking.empty() && ImGui::CollapsingHeader("The model's thinking")) {
                ImGui::TextWrapped("%s", request->draft->thinking.c_str());
            }
            if (!spec.grammar.empty() && ImGui::CollapsingHeader("Grammar")) {
                ImGui::TextUnformatted(spec.grammar.c_str());
            }

            // The reply, and the thinking when the call may think. Edits stay in this window until Submit.
            const float padding = ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::TextUnformatted("Reply");
            if (ImGui::InputTextMultiline("##reply", &_reply,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * REPLY_LINES + padding))) {
                _check.reset(); // The last check was of another text.
            }
            if (spec.thinkingMaxTokens > 0) {
                ImGui::TextUnformatted("Thinking");
                ImGui::InputTextMultiline("##thinking", &_thinking,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * THINKING_LINES + padding));
            }

            // Check (only with a grammar), Submit (whether or not the reply fits), and Withdraw (always: it releases
            // the call without a reply).
            ImGui::BeginDisabled(spec.grammar.empty());
            const bool check = ImGui::Button("Check");
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool submit = ImGui::Button("Submit");
            ImGui::SameLine();
            const bool withdraw = ImGui::Button("Withdraw");
            if (_check) {
                ImGui::Text("%d of %d answer tokens", _check->tokens, spec.maxTokens);
                if (_check->problem.empty()) {
                    ImGui::TextUnformatted("Fits the grammar");
                } else {
                    TextWrappedColored(WARNING_COLOR, _check->problem);
                }
            }

            // The prompt, as the Context window draws it, in the rest of the window.
            ImGui::SeparatorText("Prompt");
            DrawPrompt(request->prompt, _showRaw);

            // Act after drawing, one action per frame, on the request drawn (by its id). A request a cancel ended
            // since Pending() is stale: Submit and Withdraw then return false and Check nullopt, and nothing is left
            // to do.
            if (withdraw) {
                const RunModel::Busy busy = controller.Run().CurrentBusy();
                if (busy == RunModel::Busy::Turn || busy == RunModel::Busy::Regenerate) {
                    // A running turn's call (a fold of its fit loop, or of the maintenance before it, included):
                    // cancel the command, as the Play window's Cancel does, so the turn closes with TurnCancelled.
                    controller.Cancel();
                } else {
                    // Between-turn maintenance: release the fold or condense at the seat. The maintenance stops as a
                    // preempted one does, and the next turn folds what it needs.
                    seat.Withdraw(request->id);
                }
            } else if (submit) {
                seat.Submit(request->id, std::exchange(_reply, std::string()), std::exchange(_thinking, std::string()));
                _check.reset();
            } else if (check) {
                _check = seat.Check(request->id, _reply);
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 7: Add the source to the build**

In `src/app/CMakeLists.txt`, replace

```cmake
    ui/SettingsWindow.cpp
)
```

with

```cmake
    ui/SettingsWindow.cpp
    ui/ModelSeatWindow.cpp
)
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[ui]"`
Expected: all `[ui]` tests pass, the five new ones included ("The inspectors draw ..." still draws the Context window's raw view through `DrawPrompt`).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 311`.

- [ ] **Step 9: Build the app**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus`
Expected: builds with no warnings (`main.cpp` is unchanged: `AppController` wraps the llama.cpp factory).

- [ ] **Step 10: Commit**

```bash
git add src/app/ui/Windows.h src/app/ui/Ui.h src/app/ui/Ui.cpp src/app/ui/ContextWindow.cpp src/app/ui/ModelSeatWindow.cpp src/app/CMakeLists.txt tests/app/UiTests.cpp
git commit -m "Add the Model seat window" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Documentation and the full check

The architecture spec and the README describe the seat; every suite runs.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation only.

- [ ] **Step 1: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`:

Replace

```
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time.
```

with

```
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time. The one exception is the model seat's Check (a debug feature of the app): it calls the backend's `CheckGrammar` from the UI thread, only while the worker is blocked waiting for the seat.
```

Replace

```
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings | turn, llama (`main.cpp`'s backend factory) |
```

with

```
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings, the model seat | turn, llama (`main.cpp`'s backend factory) |
```

Replace

```
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
```

with

```
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
  check_grammar(grammar, text) -> "" or a reason   (whether text, tokenized as an answer, fits the grammar from start to end)
```

After the bullet that starts `- Grammar sampling checks the sampled token first:`, add the bullet:

```
- `CheckGrammar` generates nothing: `LlamaCppBackend` tokenizes the text as an answer and runs llama.cpp's grammar sampler over it, checking each token alone and then accepting it, and at the end requires the end-of-generation token to be allowed. The reason names the first rejected token and the text before it, or says "the text ends before the grammar does". An empty or unparsable grammar is a `ContractError`. The app's model seat uses it to check a hand-written reply.
```

In the UI table, after the `| Settings | ... |` row, add:

```
| Model seat | Debug: answer the engine's model calls by hand. "Answer as the model" and a Human / Draft / Model mode per call label (resolve, narrate, rewrite, lookup, review, summarize, condense); the waiting call's label, limits, prompt by layer, grammar and the model's draft; reply and thinking boxes, Check (token count and grammar fit), Submit, and Withdraw (releases the call: cancels a running turn, or ends a between-turn fold or condense) |
```

After the paragraph that starts `The windows dock over one SDL3 window`, add:

```
The model seat (design: `docs/superpowers/specs/2026-11-13-model-seat-design.md`): `AppController` owns one `app::ModelSeat` and wraps every backend the engine host builds in an `app::ModelSeatBackend` around it, so the switch and the modes survive restarts; nothing is stored in `settings.json`. With the seat off, or a call's label in Model mode, the wrapper forwards. Otherwise the call waits on the worker thread for the owner's reply (in Draft mode after the real model's answer, offered as a draft); the reply is cut to the call's limits by the real tokenizer and returned as the model's completion, streamed once, whole. A cancel, or the window's Withdraw, wakes the wait and the call ends cancelled; the UI names the request it answers by its id, so a click that loses the race with a cancel is a no-op. The reply is not constrained by the grammar, so the engine's handling of model mistakes applies to it; `core`, `context` and `turn` do not know who answers.
```

- [ ] **Step 2: The README**

In `README.md`, in "Play in the app", after the bullet that starts `- **Settings:**` (before the `--frames N` line), add:

```
- **Model seat:** a debug window for answering the engine's model calls yourself. Tick "Answer as the model" and choose, per call (resolve, narrate, rewrite, lookup, review, summarize, condense), Human (you write the reply), Draft (the model answers first and you submit or edit its answer) or Model (as usual); every call starts in Human. A waiting call shows its token limits, its prompt by layer, its grammar and the model's draft. Check counts the reply's tokens and tests it against the grammar; Submit sends it whether or not it fits, and the engine treats it as the model's reply, mistakes included; Withdraw releases the call without a reply (during a turn it cancels the turn, as Cancel does; between turns it ends that fold or condense, and the next turn folds what it needs). Nothing is saved: the seat is off whenever the app starts.
```

- [ ] **Step 3: Run the tests preset**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 311`.

- [ ] **Step 4: Run the AddressSanitizer preset**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `100% tests passed, 0 tests failed out of 311`, with no AddressSanitizer report.

- [ ] **Step 5: Build every dev target and start the app**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120`
Expected: the window opens and the app exits with code 0 once the models have loaded (`echo $?` prints `0`).

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md
git commit -m "Document the model seat" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| Part of the normal app, no separate target; switched on in its window, nothing in `settings.json` | 4 (wrapped factory), 5 (window) |
| Every label starts in Human; modes change between calls | 2 (`_modes` all Human, `SetMode`), 5 (modes table) |
| The real chat model stays loaded | 4 (the wrapper owns the real backend) |
| A grammar check in the first version | 1 (`CheckGrammar`), 2 (`Check`), 5 (Check button) |
| `morpheus_play` and replay recording out of scope | not touched |
| Seven labels and how they are recognised; `SeatLabelOf`; unrecognised summary is a ContractError | 2 (the ContractError branch is kept but unreachable; see Choices) |
| Human / Draft / Model modes | 3 |
| `LlmBackend::CheckGrammar` with its comment | 1 |
| `LlamaCppBackend` implements it with the grammar sampler, single-token candidate arrays, accept, end-of-generation check | 1 |
| `RecordingBackend` forwards; seat backend forwards; `ScriptedBackend` configurable, "" by default | 1, 3 |
| `ModelSeat`: mutex, `enabled`, `modes`, pending `SeatRequest` (id, label, prompt, spec, draft), the reply | 2 |
| At most one pending request; a second is a ContractError | 2 |
| Wait on `std::condition_variable_any` with the stop token; a cancelled wait clears the request and returns `cancelled` | 2, 3 |
| UI methods `Enabled`, `SetEnabled`, `ModeOf`, `SetMode`, `Pending`, `Submit(id, ...)`, `Check(id, ...)` (backend verdict and token count, only with a grammar) | 2 |
| Request ids: a stale id makes `Submit`/`Withdraw` return false and `Check` nullopt without throwing; an id never issued is a ContractError; the window passes the id it shows | 2 (tests: stale Submit after a stop, unpublished id), 5 (window) |
| `ModelSeat::Withdraw(id)` wakes the waiter like the stop token; the call ends cancelled | 2, 3 |
| Withdraw on a maintenance request ends that maintenance cleanly and the next turn proceeds (and folds again) | 3 (worker-level test) |
| Withdraw on a turn call cancels the command, so the turn closes with TurnCancelled; one Withdraw button replaces Cancel turn, enabled whenever a request is pending | 5 (window, headless test clicking Withdraw) |
| Turning the seat off leaves a pending request | 2 (`SetEnabled`), 3 (test) |
| `ModelSeatBackend` forwards the eight other methods | 3 |
| `Complete`: off or Model forwards; Draft runs the real call silently, returns early if cancelled; publish and wait | 3 |
| Draft unchanged returns the model's completion; otherwise cut text and thinking, real counts, prompt counts, `tokensReused` 0, `generateMs` the wait, `promptMs` and `thinkingMs` 0 | 3 |
| `onToken` once with the whole answer | 3 |
| A human reply is not grammar-constrained; the engine's mistake handling shows | 4 (non-JSON resolve test) |
| `AppController` owns the seat, `ModelSeat& Seat()`, wraps the factory (tests too); the seat outlives the backends | 4 |
| Restarts keep the toggle and modes; a rebuilt backend is wrapped around the same seat | 4 |
| Status bar says the engine waits for the model seat | 5 |
| Window in `src/app/ui/ModelSeatWindow.cpp`, declared in `Windows.h`, drawn by `Ui`, drawable headless | 5 |
| Checkbox, modes table; label, limits, prompt by layer (shared drawing, not copied), grammar collapsed, draft thinking, reply box, thinking box, Check, Submit, Withdraw; edits survive frames and reset on a new request | 5 |
| Error handling: contract breaks, stale ids are no-ops, reply never an exception, backend errors propagate | 1, 2, 3 |
| Tests: `SeatLabelOf`; `ModelSeatBackend` over `ScriptedBackend` (all listed cases, Withdraw included); `ModelSeat` Submit/Check/Withdraw by id; `[llama]` CheckGrammar; engine reaction (non-JSON resolve, cancel); Withdraw on maintenance; restart; UI headless with no request, Human, Draft, Submit and Withdraw through the window | 1, 2, 3, 4, 5 |
| Documentation: architecture spec (UI section, inference section), README "Model seat" paragraph | 6 |
