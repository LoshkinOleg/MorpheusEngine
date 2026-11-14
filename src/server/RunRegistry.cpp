#include "server/RunRegistry.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "store/Sqlite.h"

namespace morpheus::server {

RunRegistry::RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log)
    : _runsDir(std::move(runsDir))
    , _settings(std::move(settings))
    , _makeBackend(std::move(makeBackend))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_makeBackend) && static_cast<bool>(_log),
                     "RunRegistry: a backend factory and a log are needed");
    // The runs folder is the operator's choice (--runs): one that cannot be made is a configuration error.
    std::error_code error;
    std::filesystem::create_directories(_runsDir, error);
    if (error || !std::filesystem::is_directory(_runsDir)) {
        throw core::DataError(std::format("runs folder '{}': {}", core::PathText(_runsDir),
                                          error ? error.message() : std::string("it is not a folder")));
    }
}

RunRegistry::~RunRegistry()
{
    // Every worker stops at once; then _runs' destruction joins them one by one.
    StopAll();
}

void RunRegistry::CheckRunId(std::string_view runId)
{
    const bool allowed = std::all_of(runId.begin(), runId.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (runId.empty() || runId.size() > MAX_RUN_ID_LENGTH || !allowed) {
        throw core::DataError(std::format("run_id: '{}' is not 1 to {} characters of A-Z, a-z, 0-9, '_' and '-'", runId,
                                          MAX_RUN_ID_LENGTH));
    }
}

std::filesystem::path RunRegistry::RunFile(std::string_view runId) const
{
    CheckRunId(runId);
    return _runsDir / (std::string(runId) + std::string(RUN_FILE_EXTENSION));
}

bool RunRegistry::Open(const std::string& runId, turn::HostData data, std::uint64_t seed)
{
    const std::filesystem::path file = RunFile(runId);
    std::scoped_lock opening(_openMutex);

    // A run opened in this process reopens on its own worker, after its queued jobs.
    if (RunWorker* pWorker = Find(runId); pWorker != nullptr) {
        pWorker->Reopen(std::move(data), seed);
        return false;
    }

    {
        std::scoped_lock lock(_mutex);
        // No fresh, unstopped worker is born during shutdown.
        if (_stopping) {
            throw inference::InferenceError("the server is stopping");
        }
        // Ids differing only in case name one file on case-insensitive file systems: never two workers on it.
        for (const auto& [otherId, pOther] : _runs) {
            const bool sameIgnoringCase = std::ranges::equal(runId, otherId, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            if (sameIgnoringCase) {
                throw core::DataError(std::format("run_id: '{}' differs only in case from the opened run '{}'; run ids "
                                                  "name files, which may not tell case apart",
                                                  runId, otherId));
            }
        }
    }

    // Otherwise it gets a worker, which opens the file or creates it. Nothing is registered when that throws.
    std::error_code error;
    const bool existed = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    auto pWorker = std::make_unique<RunWorker>(runId, file, std::move(data), _settings, seed, _makeBackend(), _log);
    // Checked again: StopAll takes only _mutex, so it may have run while the worker was being built. An unregistered
    // worker is idle; letting it go here stops and joins it.
    std::scoped_lock lock(_mutex);
    if (_stopping) {
        throw inference::InferenceError("the server is stopping");
    }
    _runs.emplace(runId, std::move(pWorker));
    return !existed;
}

RunWorker* RunRegistry::Find(std::string_view runId) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _runs.find(runId);
    return it == _runs.end() ? nullptr : it->second.get();
}

void RunRegistry::StopAll()
{
    std::scoped_lock lock(_mutex);
    _stopping = true;
    for (auto& [runId, pWorker] : _runs) {
        pWorker->RequestStop();
    }
}

} // namespace morpheus::server
