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
