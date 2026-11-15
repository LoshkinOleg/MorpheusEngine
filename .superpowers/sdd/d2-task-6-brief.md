### Task 6: `RunWorker`: one run's thread, queue, session and background folding

**Files:**
- Create: `src/server/RunWorker.h`, `src/server/RunWorker.cpp`
- Modify: `src/server/CMakeLists.txt` (add `RunWorker.cpp`)
- Create: `tests/server/ServerTestSupport.h`
- Test: `tests/server/RunWorkerTests.cpp`

**Interfaces:**
- Consumes: `turn::HostedSession` (`Open`, `Maintain`, `Meta`, readers), `turn::HostData`, `turn::HostedWorld(const HostData&)`, `turn::HostDataHash(const HostData&)` (`src/turn/HostedWorld.h`), `server::SlotBackend`, `server::SlotPool::UPKEEP_SLOT`, `server::Log` (Task 4), `inference::InferenceError`.
- Produces:
  - `class server::RunWorker` with `using Job = std::function<void(turn::HostedSession& session, SlotBackend& backend, std::stop_token stop)>;`, constructor `RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings, std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log)`, `const std::string& RunId() const`, `std::future<void> Post(Job job)`, `void Call(Job job)`, `void Reopen(turn::HostData data, std::uint64_t seed)`, `void RequestStop()`.
  - Jobs refused because the worker stops fail with `inference::InferenceError` whose message contains `"the server is stopping"`.
  - `tests/server/ServerTestSupport.h` with `test::WaitUntil` and `test::MakeWorker` (later tasks add to this file).

- [ ] **Step 1: Write the test helpers**

Create `tests/server/ServerTestSupport.h`:

```cpp
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "server/RunWorker.h"

namespace morpheus::test {

// Polls `condition` every 20 ms until it holds or `limit` passes; returns whether it held.
inline bool WaitUntil(const std::function<bool()>& condition,
                      std::chrono::milliseconds limit = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// A run worker named `runId` on run file `file`, with open.json's host data, HostedSettings and seed 42, on a
// scripted backend that answers with `responder`. `*ppBackend` gets that backend's address, so a test can reach it
// from a job (on the worker's thread, where the backend is used). Log lines are dropped.
inline std::unique_ptr<server::RunWorker> MakeWorker(const std::string& runId, const std::filesystem::path& file,
                                                     ScriptedBackend::Responder responder,
                                                     ScriptedBackend** ppBackend = nullptr)
{
    auto pBackend = std::make_unique<ScriptedBackend>(16384, std::move(responder));
    if (ppBackend != nullptr) {
        *ppBackend = pBackend.get();
    }
    return std::make_unique<server::RunWorker>(runId, file, FixtureHostData(), HostedSettings(), 42, std::move(pBackend),
                                               [](const std::string&) {});
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/RunWorkerTests.cpp`. Catch2's checks are not thread-safe: a job only records what it saw into variables, and the test checks them after the job's future (or `Call`) has returned.

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerTestSupport.h"
#include "server/SlotBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A job that does nothing.
void Nothing(turn::HostedSession&, server::SlotBackend&, std::stop_token)
{
}

} // namespace

TEST_CASE("RunWorker opens its run and runs jobs one at a time, in arrival order, on its own thread",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_order");
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_order", file, script.Responder());
    CHECK(worker->RunId() == "worker_order");
    CHECK(std::filesystem::exists(file));

    std::vector<int> order;
    std::thread::id jobThread;
    std::vector<std::future<void>> done;
    for (int i = 0; i < 5; ++i) {
        done.push_back(worker->Post([&order, &jobThread, i](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
            order.push_back(i);
            jobThread = std::this_thread::get_id();
        }));
    }
    for (std::future<void>& job : done) {
        job.get();
    }
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
    CHECK(jobThread != std::this_thread::get_id());
}

TEST_CASE("A job's exception reaches its caller, and the worker goes on", "[hosted-server][worker]")
{
    test::HostedScript script;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_throw", test::HostedRunFile("worker_throw"), script.Responder());
    CHECK_THROWS_AS(worker->Call([](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
                        throw core::DataError("speaker: no such actor");
                    }),
                    core::DataError);
    bool ran = false;
    worker->Call([&ran](turn::HostedSession&, server::SlotBackend&, std::stop_token) { ran = true; });
    CHECK(ran);
}

TEST_CASE("A RunWorker that cannot open its run throws the open's error and leaves no file", "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_bad_data");
    turn::HostData bad = test::FixtureHostData();
    bad.catalog = 42; // Not an action catalog: HostedWorld's DataError, before the file is touched.
    CHECK_THROWS_AS(server::RunWorker("worker_bad_data", file, bad, test::HostedSettings(), 42,
                                      std::make_unique<test::ScriptedBackend>(16384, script.Responder()),
                                      [](const std::string&) {}),
                    core::DataError);
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Between jobs the worker folds ended conversations on the upkeep slot; a job's calls use the slot it sets",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_fold", test::HostedRunFile("worker_fold"), script.Responder(), &pBackend);

    std::string id;
    bool played = false;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend& backend, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        backend.SetSlot(2);
        played = session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop).has_value();
        session.EndConversation(id);
    });
    REQUIRE(played);

    // Folding happens in the background once the queue is empty: wait until the conversation's turn is folded.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);

    // The turn's two calls ran on slot 2, the fold's on the upkeep slot.
    std::vector<int> slots;
    std::vector<bool> folds;
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        for (const test::ScriptedBackend::Call& call : pBackend->Calls()) {
            slots.push_back(call.spec.slot);
            folds.push_back(call.prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION);
        }
    });
    REQUIRE(slots.size() >= 3);
    CHECK(slots[0] == 2);
    CHECK(slots[1] == 2);
    for (std::size_t i = 2; i < slots.size(); ++i) {
        CHECK(slots[i] == server::SlotPool::UPKEEP_SLOT);
        CHECK(folds[i]);
    }
}

TEST_CASE("A job stops a running fold, and the fold runs again after it", "[hosted-server][worker]")
{
    // The fold's summary streams slowly: 9 pieces, 100 ms each, once the delay is set.
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    std::atomic<int> foldCalls = 0;
    test::ScriptedBackend::Responder responder = [&](const inference::Prompt& prompt,
                                                     const inference::GenerationSpec& spec) -> std::string {
        if (prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION) {
            ++foldCalls;
            return "Summary of a long talk at the inn.";
        }
        return script.Responder()(prompt, spec);
    };
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_fold_stop", test::HostedRunFile("worker_fold_stop"), responder, &pBackend);
    std::string id;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop);
        pBackend->SetPieceDelay(std::chrono::milliseconds(100));
        session.EndConversation(id);
    });

    // While the fold streams, a job arrives: the fold stops at its next piece, and the job runs at once.
    REQUIRE(test::WaitUntil([&] { return foldCalls.load() >= 1; }));
    const auto start = std::chrono::steady_clock::now();
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        pBackend->SetPieceDelay(std::chrono::milliseconds(0));
    });
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));

    // Then the fold runs again, now without the delay, and lands.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);
    CHECK(foldCalls.load() >= 2);
}

TEST_CASE("Stopping the worker stops a running turn, which commits nothing, and refuses later jobs",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"A long answer that streams slowly, piece by piece."};
    const std::filesystem::path file = test::HostedRunFile("worker_stop");
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_stop", file, script.Responder(), &pBackend);

    std::atomic<bool> streaming = false;
    std::string id;
    std::optional<core::HostedTurn> result;
    std::future<void> turn = worker->Post([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        pBackend->SetPieceDelay(std::chrono::milliseconds(100));
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streaming](std::string_view) { streaming = true; };
        result = session.RunTurn(test::Request(id, test::HULDA, "Hello."), callbacks, stop);
    });
    REQUIRE(test::WaitUntil([&] { return streaming.load(); }));
    worker->RequestStop();
    turn.get(); // The job returned: the stop cancelled its turn.
    CHECK_FALSE(result.has_value());
    CHECK_THROWS_AS(worker->Call(Nothing), inference::InferenceError);
    CHECK_THROWS_WITH(worker->Call(Nothing), ContainsSubstring("the server is stopping"));
    worker.reset();

    // Nothing was committed.
    test::ScriptedBackend fresh(16384, script.Responder());
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, fresh);
    CHECK(reopened.Turns(id).empty());
}

TEST_CASE("Reopen: the same data changes nothing, changed data applies, and a failed reopen leaves the run as it was",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_reopen", test::HostedRunFile("worker_reopen"), script.Responder(), &pBackend);
    // What the run looks like now: its stored host data's hash, and whether conversation `id` is still there.
    std::string id;
    auto look = [&worker, &id] {
        std::pair<std::string, bool> seen;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            seen = {session.Meta().worldHash, session.Conversation(id).has_value()};
        });
        return seen;
    };
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    });
    const std::string original = turn::HostDataHash(test::FixtureHostData());
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // The same data: nothing changes.
    worker->Reopen(test::FixtureHostData(), 42);
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // Changed data (a mod update): the run reopens with it and keeps its conversations.
    turn::HostData changed = test::FixtureHostData();
    changed.lore.push_back({"The mill", "The mill burned down last winter.", false});
    worker->Reopen(changed, 42);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Bad data: HostedWorld's DataError, and the open run is untouched.
    turn::HostData bad = changed;
    bad.catalog = 42;
    CHECK_THROWS_AS(worker->Reopen(bad, 42), core::DataError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Data whose open fails (its new lore cannot be embedded): the error, and the run opens again as it was.
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) { pBackend->SetEmbedLimit(64); });
    turn::HostData unembeddable = changed;
    unembeddable.lore.push_back({"The road", std::string(200, 'z'), false});
    CHECK_THROWS_AS(worker->Reopen(unembeddable, 42), inference::InferenceError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/RunWorker.h'`.

- [ ] **Step 4: Write `RunWorker`**

Create `src/server/RunWorker.h`:

```cpp
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
// own backend, and runs the run's jobs one at a time, in arrival order. While no job waits, it folds the run's ended
// conversations in the background (HostedSession::Maintain, on the upkeep slot); a new job stops that, and it resumes
// once the queue is empty again. HostedSession's contract (one caller at a time) holds because only this thread calls
// it. Every public method is thread-safe.
class RunWorker {
public:
    // A job: the run's session, the backend its calls go through (the job sets the slot), and a stop token that fires
    // when the worker stops.
    using Job = std::function<void(turn::HostedSession& session, SlotBackend& backend, std::stop_token stop)>;

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
    std::stop_source _maintenanceStop = {}; // Stops the running Maintain; a new job and RequestStop request it.
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

    // Queues `job` after the jobs before it and returns its completion: the future rethrows what the job threw. A run
    // left closed by a failed Reopen fails the job with InferenceError naming the run. After RequestStop the job never
    // runs, and the future holds InferenceError "the server is stopping".
    std::future<void> Post(Job job);

    // Post, then wait: rethrows what the job threw.
    void Call(Job job);

    // Opens the run again with `data`, on the worker's thread after the queued jobs, and waits. The same host data
    // (equal content hash) changes nothing. Changed data is checked first (HostedWorld's DataError; the open run is
    // untouched); then the session closes and the run opens again (HostedSession::Open re-embeds the changed lore,
    // then replaces the stored host data). If that open fails, the run opens again with its previous data and the
    // error is rethrown; if even that fails, the run stays closed (logged) until a Reopen succeeds. `seed` matters only
    // when the file is missing.
    void Reopen(turn::HostData data, std::uint64_t seed);

    // Stops the worker without waiting: the running job's stop token and a running Maintain fire, and queued and later
    // jobs fail with InferenceError "the server is stopping". The destructor joins.
    void RequestStop();

private:
    // Queues `work` and returns its completion; refused (InferenceError "the server is stopping") after RequestStop.
    std::future<void> Enqueue(std::function<void(std::stop_token)> work);
};

} // namespace morpheus::server
```

Create `src/server/RunWorker.cpp`:

```cpp
#include "server/RunWorker.h"

#include <exception>
#include <format>
#include <string_view>
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
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, stop, [this] { return !_queue.empty() || _maintenanceDue; });
                if (stop.stop_requested()) {
                    return; // RequestStop failed the queued jobs; Post refuses new ones.
                }
                if (!_queue.empty()) {
                    entry.emplace(std::move(_queue.front()));
                    _queue.pop_front();
                } else {
                    _maintenanceStop = std::stop_source();
                    maintenanceToken = _maintenanceStop.get_token();
                }
            }

            if (!entry) {
                // Folding, on the upkeep slot. A new job or RequestStop stops it; it then runs again once the queue is
                // empty. A failure (the model unreachable, a summary cap too small) must not stop the run's turns: it
                // is logged, and folding is tried again after the next job.
                bool finished = true;
                if (_session) {
                    _backend.SetSlot(SlotPool::UPKEEP_SLOT);
                    try {
                        finished = _session->Maintain(maintenanceToken);
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

std::future<void> RunWorker::Post(Job job)
{
    MORPHEUS_REQUIRE(static_cast<bool>(job), "RunWorker::Post: no job");
    // A job needs the session; a run left closed by a failed Reopen has none.
    return Enqueue([this, job = std::move(job)](std::stop_token stop) {
        if (!_session) {
            throw inference::InferenceError(
                std::format("run '{}' is closed: it could not be opened again after a failed reopen", _runId));
        }
        job(*_session, _backend, stop);
    });
}

void RunWorker::Call(Job job)
{
    Post(std::move(job)).get();
}

void RunWorker::Reopen(turn::HostData data, std::uint64_t seed)
{
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
    }).get();
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

std::future<void> RunWorker::Enqueue(std::function<void(std::stop_token)> work)
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
        // Work comes first: a running Maintain stops now, and runs again once the queue is empty.
        _maintenanceStop.request_stop();
    }
    _wake.notify_one();
    return done;
}

} // namespace morpheus::server
```

Notes for the implementer:
- `std::condition_variable_any::wait(lock, stop, predicate)` returns when the predicate holds or the stop is requested; `_thread.request_stop()` wakes it.
- The constructor's member initializer order must match the declaration order (`-Wreorder` is an error).
- If `HostedSession` has no move assignment, `std::optional::emplace` (used above) is still fine: it destroys and constructs in place. `HostedSession::Open` returns by value.

Add `RunWorker.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[worker]"`
Expected: `All tests passed`. Run it three more times to check that the timing test is steady: `for i in 1 2 3; do ./build/tests/bin/Debug/morpheus_tests.exe "[worker]" | tail -1; done`.

- [ ] **Step 6: Commit**

```bash
git add src/server/RunWorker.h src/server/RunWorker.cpp src/server/CMakeLists.txt tests/server/ServerTestSupport.h tests/server/RunWorkerTests.cpp
git commit -m "RunWorker: a run's own thread, job queue and session, folding in the background between jobs"
```

---

