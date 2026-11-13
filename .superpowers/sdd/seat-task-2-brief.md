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

