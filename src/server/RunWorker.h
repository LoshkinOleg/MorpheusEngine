#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "server/ServerLog.h"
#include "server/SlotBackend.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

namespace morpheus::server {

// One opened run of morpheus_server (spec, section 2): a thread of its own that owns the run's HostedSession and its
// own backend, and runs the run's jobs one at a time, in arrival order. While no job waits, it keeps the run's memories
// in the background (HostedSession::Maintain, on the upkeep slot); a preempting job (a turn) stops that when it is
// queued, every other job asks it to yield and waits only for its current step (one NPC's catch-up, one share), and
// Maintain resumes once the queue is empty again. HostedSession's contract (one caller at a time) holds because only
// this thread calls it. Every public method is thread-safe.
class RunWorker {
public:
    // A job: the run's session, the backend its calls go through (the job sets the slot), and a stop token that fires
    // when the worker stops.
    using Job = std::function<void(turn::HostedSession& session, SlotBackend& backend, std::stop_token stop)>;

    // Whether a job stops a running background fold when it is queued (Post): only a turn does, so a host that polls
    // or records events often cannot keep folding from finishing. Other jobs ask Maintain to yield and wait behind its
    // current step, which finishes.
    enum class Preempt { None, Fold };

private:
    // A queued piece of work and its completion. Post's jobs need the session; Reopen's work replaces it.
    struct Entry {
        std::function<void(std::stop_token)> work = {};
        std::promise<void> done = {};
    };

    std::string _runId = {};
    std::filesystem::path _file = {};
    core::Settings _settings = {};
    turn::HostData _data = {}; // The host data the session was last opened with.
    std::uint64_t _seed = 0;
    std::unique_ptr<inference::LlmBackend> _pModels = nullptr;
    SlotBackend _backend; // Wraps *_pModels; set by the constructor.
    Log _log = {};
    std::optional<turn::HostedSession> _session = {}; // Empty only while closed after a failed Reopen.
    std::mutex _mutex = {};                           // Guards the members below.
    std::condition_variable_any _wake = {};
    std::deque<Entry> _queue = {};
    bool _maintenanceDue = true;           // Folding may have work: set after each job, cleared once Maintain is done.
    std::stop_source _maintenanceStop = {}; // Stops the running Maintain; a preempting job and RequestStop request it.
    std::stop_source _maintenanceYield = {}; // Asks the running Maintain to return between steps; every job does.
    bool _stopping = false;
    std::jthread _thread = {}; // Last: joined first on destruction, before the session and the backend go.

public:
    // Opens the run on the worker's thread (HostedSession::Open on `file` with `data`, `settings` and `seed`, through
    // `backend`) and waits for it: Open's exception (DataError, StoreError, InferenceError) is rethrown here, and no
    // worker is left. `log` gets the worker's lines (folding failures). Throws ContractError for a null backend or an
    // empty log.
    RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings,
              std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log);
    // Stops (RequestStop) and joins the thread.
    ~RunWorker();
    RunWorker(const RunWorker&) = delete;
    RunWorker& operator=(const RunWorker&) = delete;

    const std::string& RunId() const;

    // Queues `job` after the jobs before it and returns its completion: the future rethrows what the job threw. With
    // Preempt::Fold a running Maintain stops at once (even with jobs queued before this one); otherwise Maintain
    // returns after its current step and the job waits only for that. A run left closed by a failed Reopen fails the
    // job with InferenceError naming the run. After RequestStop the job never runs, and the future holds
    // InferenceError "the server is stopping".
    std::future<void> Post(Job job, Preempt preempt = Preempt::None);

    // Post (not preempting), then wait: rethrows what the job threw. Throws ContractError on the worker's own thread
    // (from inside a job), where it would wait for itself.
    void Call(Job job);

    // Opens the run again with `data`, on the worker's thread after the queued jobs (not preempting), and waits. The same host data
    // (equal content hash) changes nothing. Changed data is checked first (HostedWorld's DataError; the open run is
    // untouched); then the session closes and the run opens again (HostedSession::Open re-embeds the changed lore,
    // then replaces the stored host data). If that open fails, the run opens again with its previous data and the
    // error is rethrown; if even that fails, the run stays closed (logged) until a Reopen succeeds. `seed` matters only
    // when the file is missing. Throws ContractError on the worker's own thread, where it would wait for itself.
    void Reopen(turn::HostData data, std::uint64_t seed);

    // Stops the worker without waiting: the running job's stop token and a running Maintain fire, and queued and later
    // jobs fail with InferenceError "the server is stopping". The destructor joins.
    void RequestStop();

private:
    // Queues `work` and returns its completion; refused (InferenceError "the server is stopping") after RequestStop.
    // Any work asks a running Maintain to yield after its current step; Preempt::Fold stops it now.
    std::future<void> Enqueue(std::function<void(std::stop_token)> work, Preempt preempt);
};

} // namespace morpheus::server
