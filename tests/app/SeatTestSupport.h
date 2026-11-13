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
