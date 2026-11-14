#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::server {

// One event of a turn's stream (morpheus_server spec, section 1, "The turn stream"): action, token, sentence, done or
// error, and its JSON.
struct StreamEvent {
    std::string name = {};
    nlohmann::json data = nlohmann::json::object();
};

// A turn's events on their way from the run's worker (Push, End) to the HTTP handler that writes them (Next, Cancel).
// The handler's side also tells the turn to stop: Cancel fires StopToken, which the turn runs under. Thread-safe.
class TurnStream {
private:
    mutable std::mutex _mutex = {};
    std::condition_variable _changed = {};
    std::deque<StreamEvent> _events = {};
    bool _ended = false;
    std::stop_source _stop = {};

public:
    // Queues an event. Throws ContractError after End.
    void Push(StreamEvent event);
    // No event follows.
    void End();
    // Takes the next event, waiting up to `wait` for one; nullopt when none came (Ended() tells whether one ever will).
    std::optional<StreamEvent> Next(std::chrono::milliseconds wait);
    // Whether the stream ended and every event was taken.
    bool Ended() const;
    // Waits up to `wait` for the stream to start: true once an event is queued or the stream ended.
    bool WaitStarted(std::chrono::milliseconds wait);
    // The next event without taking it; nullopt when none is queued.
    std::optional<StreamEvent> Front() const;
    // The client left, or the server stops: the turn's stop token fires.
    void Cancel();
    // What the turn runs under.
    std::stop_token StopToken() const;
};

} // namespace morpheus::server
