#include "app/model/ModelSeat.h"

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "core/Errors.h"
#include "grammar/Check.h"
#include "inference/Llm.h"

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

SeatLabel SeatLabelOf(const inference::GenerationSpec& spec)
{
    // The tag the turn layer set: each owned call has its own label.
    const std::string& tag = spec.callTag;
    if (tag == "resolve") {
        return SeatLabel::Resolve;
    }
    if (tag == "narrate") {
        return SeatLabel::Narrate;
    }
    if (tag == "renarrate") {
        return SeatLabel::Rewrite;
    }
    if (tag == "lookup") {
        return SeatLabel::Lookup;
    }
    if (tag == "review") {
        return SeatLabel::Review;
    }
    if (tag == "summarize") {
        return SeatLabel::Summarize;
    }
    if (tag == "condense") {
        return SeatLabel::Condense;
    }
    throw core::ContractError(std::format("SeatLabelOf: no seat label for the call tag '{}'", tag));
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
    check.problem = grammar::Check(_pending->spec.grammar, text);
    // Under llama-server the count is a POST /tokenize on this (the UI) thread, which fails when the chat server is
    // down or restarting. That is reported as the problem, with no count, so the app carries on and the owner's
    // reply stays in the window; only InferenceError is caught, so an engine bug still fails loudly. A hung server
    // blocks this check (and the window) for up to the request's read timeout (60 s), and once more for its one retry.
    try {
        check.tokens = _pBackend->CountTokens(text);
    } catch (const inference::InferenceError& error) {
        const std::string failure = std::string("cannot count tokens: ") + error.what();
        check.problem = check.problem.empty() ? failure : check.problem + "; " + failure;
    }
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
