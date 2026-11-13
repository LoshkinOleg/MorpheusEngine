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
    // Let the worker take the withdrawal and leave the maintenance. A command posted before that would find the
    // maintenance still running, which then stays due and reruns ahead of that command (Worker's contract), and
    // its MemoryUpdated would come with the turn's events.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

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
