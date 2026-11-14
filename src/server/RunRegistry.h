#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerLog.h"
#include "turn/HostedWorld.h"

namespace morpheus::server {

// The runs morpheus_server has opened (spec, section 2): each run id's file in the runs folder and its worker. A run
// stays open until the registry is destroyed. Thread-safe.
class RunRegistry {
public:
    // Makes each run's own backend (backends are not thread-safe, so runs do not share one).
    using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>()>;

    static constexpr std::size_t MAX_RUN_ID_LENGTH = 64;
    static constexpr std::string_view RUN_FILE_EXTENSION = ".morpheus";

private:
    std::filesystem::path _runsDir = {};
    core::Settings _settings = {};
    BackendFactory _makeBackend = {};
    Log _log = {};
    std::mutex _openMutex = {};     // One open at a time: two opens of one new run make one worker.
    mutable std::mutex _mutex = {}; // Guards _runs and _stopping.
    std::map<std::string, std::unique_ptr<RunWorker>, std::less<>> _runs = {};
    bool _stopping = false;         // Set by StopAll, under _mutex: no run opens after it.

public:
    // Creates `runsDir` when it is missing; DataError naming it when it cannot. Throws ContractError for an empty
    // factory or log.
    RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log);
    // Stops every worker (StopAll), then joins them.
    ~RunRegistry();
    RunRegistry(const RunRegistry&) = delete;
    RunRegistry& operator=(const RunRegistry&) = delete;

    // Throws DataError "run_id: ..." unless `runId` is 1 to MAX_RUN_ID_LENGTH characters of A-Z, a-z, 0-9, '_' and '-'.
    static void CheckRunId(std::string_view runId);

    // "<runs dir>/<run_id>.morpheus". Checks the id first (CheckRunId).
    std::filesystem::path RunFile(std::string_view runId) const;

    // Opens run `runId` with `data`. The first open in this process makes its worker, which creates the file (with
    // `seed` for sampling) or reopens it; a later open reopens it on its worker (RunWorker::Reopen). Returns whether the
    // file was created. Throws what the open throws (DataError, StoreError, InferenceError, including the backend
    // factory's); nothing is registered then. Opens take turns, so a long open (lore to embed) delays other opens, not
    // other runs' requests. DataError "run_id: ..." for an id that differs only in case from an opened run's (run ids name
    // files, which may not tell case apart); InferenceError "the server is stopping" after StopAll.
    bool Open(const std::string& runId, turn::HostData data, std::uint64_t seed);

    // The worker of a run opened in this process, or nullptr. It lives until the registry is destroyed.
    RunWorker* Find(std::string_view runId) const;

    // Stops every worker without waiting (RunWorker::RequestStop): running turns and folds stop, and later jobs fail.
    void StopAll();
};

} // namespace morpheus::server
