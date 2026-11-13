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

