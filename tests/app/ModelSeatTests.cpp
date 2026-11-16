#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "app/SeatTestSupport.h"
#include "app/model/ModelSeat.h"
#include "core/Errors.h"
#include "fakes/FakeBackend.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"

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

// A backend whose tokenizer cannot be reached, as llama-server's /tokenize when the chat server is down.
class UnreachableTokenizerBackend final : public test::FakeBackend {
public:
    UnreachableTokenizerBackend()
        : FakeBackend(16384)
    {
    }

    int CountTokens(std::string_view) override
    {
        throw inference::InferenceError("cannot reach http://127.0.0.1:8080/tokenize");
    }
};

} // namespace

TEST_CASE("SeatLabelOf labels each call by its tag", "[model_seat]")
{
    auto labelOf = [](const std::string& tag) {
        inference::GenerationSpec spec;
        spec.callTag = tag;
        return app::SeatLabelOf(spec);
    };
    CHECK(labelOf("resolve") == app::SeatLabel::Resolve);
    CHECK(labelOf("narrate") == app::SeatLabel::Narrate);
    CHECK(labelOf("renarrate") == app::SeatLabel::Rewrite);
    CHECK(labelOf("lookup") == app::SeatLabel::Lookup);
    CHECK(labelOf("review") == app::SeatLabel::Review);
    CHECK(labelOf("summarize") == app::SeatLabel::Summarize);
    CHECK(labelOf("condense") == app::SeatLabel::Condense);
    CHECK_THROWS_WITH(labelOf(""), "SeatLabelOf: no seat label for the call tag ''");
    CHECK_THROWS_WITH(labelOf("speak"), "SeatLabelOf: no seat label for the call tag 'speak'");
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

    // Check fits the reply to the request's grammar with grammar::Check, and counts its tokens with the backend that
    // published the request.
    const std::optional<app::SeatCheck> checked = seat.Check(published.id, "maybe");
    REQUIRE(checked);
    CHECK(checked->problem == "the grammar rejects 'm' at the start");
    CHECK(checked->tokens == backend.CountTokens("maybe"));
    CHECK(seat.Check(published.id, "yes")->problem.empty());

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
    CHECK(seat.Submit(published.id, "You walk.", ""));
    CHECK(waiting.Get());
}

TEST_CASE("ModelSeat::Check reports a token count that fails as the problem, and the request stays pending",
          "[model_seat]")
{
    app::ModelSeat seat;
    UnreachableTokenizerBackend backend;
    app::SeatRequest request;
    request.label = app::SeatLabel::Resolve;
    request.prompt = {{{inference::Role::User, "Is it yes?"}}, 0};
    request.spec.grammar = R"(root ::= "yes" | "no")";
    request.spec.maxTokens = 8;
    test::AsyncCall<std::optional<app::SeatReply>> waiting(
        [&](std::stop_token stop) { return seat.Await(request, backend, stop); });

    // The reply fits the grammar, but its tokens cannot be counted: Check says so instead of throwing.
    const app::SeatRequest published = test::WaitForRequest(seat);
    std::optional<app::SeatCheck> checked;
    REQUIRE_NOTHROW(checked = seat.Check(published.id, "yes"));
    REQUIRE(checked);
    CHECK(checked->problem == "cannot count tokens: cannot reach http://127.0.0.1:8080/tokenize");
    CHECK(checked->tokens == 0);

    // A reply that also breaks the grammar reports both.
    const std::optional<app::SeatCheck> both = seat.Check(published.id, "maybe");
    REQUIRE(both);
    CHECK(both->problem ==
          "the grammar rejects 'm' at the start; cannot count tokens: cannot reach http://127.0.0.1:8080/tokenize");

    // The request is still pending, so the owner's reply can still be submitted.
    REQUIRE(seat.Pending());
    CHECK(seat.Pending()->id == published.id);
    CHECK(seat.Submit(published.id, "yes", ""));
    CHECK(waiting.Get()->text == "yes");
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
