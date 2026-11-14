#include "server/TurnStream.h"

#include <utility>

#include "core/Errors.h"

namespace morpheus::server {

void TurnStream::Push(StreamEvent event)
{
    {
        std::scoped_lock lock(_mutex);
        MORPHEUS_REQUIRE(!_ended, "TurnStream::Push: the stream has ended");
        _events.push_back(std::move(event));
    }
    _changed.notify_all();
}

void TurnStream::End()
{
    {
        std::scoped_lock lock(_mutex);
        _ended = true;
    }
    _changed.notify_all();
}

std::optional<StreamEvent> TurnStream::Next(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
    if (_events.empty()) {
        return std::nullopt;
    }
    StreamEvent event = std::move(_events.front());
    _events.pop_front();
    return event;
}

bool TurnStream::Ended() const
{
    std::scoped_lock lock(_mutex);
    return _ended && _events.empty();
}

bool TurnStream::WaitStarted(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    return _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
}

std::optional<StreamEvent> TurnStream::Front() const
{
    std::scoped_lock lock(_mutex);
    if (_events.empty()) {
        return std::nullopt;
    }
    return _events.front();
}

void TurnStream::Cancel()
{
    _stop.request_stop();
}

std::stop_token TurnStream::StopToken() const
{
    return _stop.get_token();
}

} // namespace morpheus::server
