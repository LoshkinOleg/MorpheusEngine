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
