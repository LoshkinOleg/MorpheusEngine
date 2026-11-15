#include "server/RunWorker.h"

#include <exception>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "core/Errors.h"
#include "server/SlotPool.h"

namespace morpheus::server {

namespace {

// What a job refused by a stopping worker fails with.
constexpr std::string_view STOPPING_MESSAGE = "the server is stopping";

} // namespace

RunWorker::RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings,
                     std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log)
    : _runId(std::move(runId))
    , _file(std::move(file))
    , _settings(std::move(settings))
    , _data(std::move(data))
    , _seed(seed)
    , _pModels(std::move(backend))
    , _backend([this]() -> inference::LlmBackend& {
        MORPHEUS_REQUIRE(_pModels != nullptr, "RunWorker: a run needs a backend");
        return *_pModels;
    }())
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_log), "RunWorker: a run needs a log");

    // The run opens on the worker's own thread, which then serves it; the constructor waits for the open.
    std::promise<void> opened;
    std::future<void> openedResult = opened.get_future();
    _thread = std::jthread([this, opened = std::move(opened)](std::stop_token stop) mutable {
        try {
            _session.emplace(turn::HostedSession::Open(_file, _data, _settings, _seed, _backend));
            opened.set_value();
        } catch (...) {
            opened.set_exception(std::current_exception());
            return;
        }

        for (;;) {
            // The next job, or folding when no job waits and folding may have work; nothing until one or the other.
            std::optional<Entry> entry;
            std::stop_token maintenanceToken;
            std::stop_token yieldToken;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, stop, [this] { return !_queue.empty() || _maintenanceDue; });
                // RequestStop failed the queued jobs and Post refuses new ones. _stopping is checked as well as the
                // thread's stop: RequestStop sets it (and stops the current Maintain) under the lock before it stops
                // the thread, so a Maintain started in between would get a fresh stop source nobody stops.
                if (stop.stop_requested() || _stopping) {
                    return;
                }
                if (!_queue.empty()) {
                    entry.emplace(std::move(_queue.front()));
                    _queue.pop_front();
                } else {
                    // A fresh pass: neither stopped nor asked to yield until work is queued during it.
                    _maintenanceStop = std::stop_source();
                    maintenanceToken = _maintenanceStop.get_token();
                    _maintenanceYield = std::stop_source();
                    yieldToken = _maintenanceYield.get_token();
                }
            }

            if (!entry) {
                // Folding, on the upkeep slot. A preempting job or RequestStop stops it; any other job queued
                // meanwhile makes it return after its current step. Either way it runs again, resuming from its
                // marks, once the queue is empty. A failure (the model unreachable, a summary cap too small) must not
                // stop the run's turns: it is logged, and folding is tried again after the next job.
                bool finished = true;
                if (_session) {
                    _backend.SetSlot(SlotPool::UPKEEP_SLOT);
                    try {
                        finished = _session->Maintain(maintenanceToken, yieldToken);
                    } catch (const std::exception& error) {
                        _log(std::format("run {}: background folding failed: {}", _runId, error.what()));
                    }
                }
                if (finished) {
                    std::scoped_lock lock(_mutex);
                    _maintenanceDue = false;
                }
                continue;
            }

            // A job: run it and report its end; afterwards folding looks again.
            try {
                entry->work(stop);
                entry->done.set_value();
            } catch (...) {
                entry->done.set_exception(std::current_exception());
            }
            std::scoped_lock lock(_mutex);
            _maintenanceDue = true;
        }
    });
    openedResult.get(); // Rethrows Open's exception; the thread has returned, and _thread's destructor joins it.
}

RunWorker::~RunWorker()
{
    RequestStop();
}

const std::string& RunWorker::RunId() const
{
    return _runId;
}

std::future<void> RunWorker::Post(Job job, Preempt preempt)
{
    MORPHEUS_REQUIRE(static_cast<bool>(job), "RunWorker::Post: no job");
    // A job needs the session; a run left closed by a failed Reopen has none.
    return Enqueue(
        [this, job = std::move(job)](std::stop_token stop) {
            if (!_session) {
                throw inference::InferenceError(
                    std::format("run '{}' is closed: it could not be opened again after a failed reopen", _runId));
            }
            job(*_session, _backend, stop);
        },
        preempt);
}

void RunWorker::Call(Job job)
{
    // On the worker's own thread (from inside a job) the wait could never end: the job waits for itself.
    MORPHEUS_REQUIRE(std::this_thread::get_id() != _thread.get_id(),
                     "RunWorker::Call: called from the worker's own thread, it would wait for itself");
    Post(std::move(job)).get();
}

void RunWorker::Reopen(turn::HostData data, std::uint64_t seed)
{
    // On the worker's own thread (from inside a job) the wait could never end: the job waits for itself.
    MORPHEUS_REQUIRE(std::this_thread::get_id() != _thread.get_id(),
                     "RunWorker::Reopen: called from the worker's own thread, it would wait for itself");
    // Not preempting: reopening waits for a running fold's current step, like every job but a turn.
    Enqueue([this, data = std::move(data), seed](std::stop_token) mutable {
        // Changed data is checked before the open run is touched; the same data changes nothing.
        const turn::HostedWorld check(data);
        if (_session && turn::HostDataHash(data) == _session->Meta().worldHash) {
            return;
        }
        // One session per run file: the open one closes before the run opens again.
        _session.reset();
        try {
            _session.emplace(turn::HostedSession::Open(_file, data, _settings, seed, _backend));
            _data = std::move(data);
        } catch (...) {
            // The new data could not open the run: open it again as it was, and report the new data's error.
            try {
                _session.emplace(turn::HostedSession::Open(_file, _data, _settings, _seed, _backend));
            } catch (const std::exception& error) {
                _log(std::format("run {}: cannot open again with its previous host data: {}", _runId, error.what()));
            }
            throw;
        }
    }, Preempt::None).get();
}

void RunWorker::RequestStop()
{
    // Refuse what is queued and what comes later, stop a running Maintain, then the thread: its stop token reaches the
    // running job.
    std::deque<Entry> refused;
    {
        std::scoped_lock lock(_mutex);
        _stopping = true;
        _maintenanceStop.request_stop();
        refused.swap(_queue);
    }
    for (Entry& entry : refused) {
        entry.done.set_exception(std::make_exception_ptr(inference::InferenceError(std::string(STOPPING_MESSAGE))));
    }
    _thread.request_stop();
}

std::future<void> RunWorker::Enqueue(std::function<void(std::stop_token)> work, Preempt preempt)
{
    Entry entry;
    entry.work = std::move(work);
    std::future<void> done = entry.done.get_future();
    {
        std::scoped_lock lock(_mutex);
        if (_stopping) {
            entry.done.set_exception(std::make_exception_ptr(inference::InferenceError(std::string(STOPPING_MESSAGE))));
            return done;
        }
        _queue.push_back(std::move(entry));
        // Preemption is decided here, when the work is queued, not when it reaches the front: a turn queued behind
        // jobs that wait still stops a running Maintain now. Any work asks a running Maintain to return after its
        // current step, so it waits for that step only. Maintain runs again once the queue is empty.
        _maintenanceYield.request_stop();
        if (preempt == Preempt::Fold) {
            _maintenanceStop.request_stop();
        }
    }
    _wake.notify_one();
    return done;
}

} // namespace morpheus::server
