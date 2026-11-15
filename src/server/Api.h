#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "inference/Llm.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"
#include "server/TurnStream.h"

namespace morpheus::server {

// morpheus_server's API version, on every response (spec, section 1). A breaking change bumps the major version and
// gets a CHANGELOG.md entry.
inline constexpr std::string_view API_VERSION = "1.3.0";

// An error answer's body: {"api_version", "error": {"status", "message"}}. Every error answer has this shape, those the
// HTTP layer makes itself included.
nlohmann::json ErrorBody(int status, std::string_view message);

// What /health reports about the models.
struct HealthInfo {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int slots = 0;
};

// One answer: a status and a JSON body, or, for a turn that started, the stream of its events.
struct ApiResponse {
    int status = 200;
    nlohmann::json body = nlohmann::json::object(); // Carries api_version. Unused when `stream` is set.
    std::shared_ptr<TurnStream> stream = nullptr;   // A started turn: its events, the first (action) already queued.
};

// morpheus_server's routes without sockets (spec, sections 1 and 4): a method, a path and a body in; a status and JSON,
// or a turn's stream, out. Every route, check and status lives here; HttpServer only carries bytes. A run's requests
// run on its worker (RunWorker), so requests of different runs proceed at once; /v1/tokenize and /v1/embeddings take
// turns on the utility backend. Thread-safe.
class Api {
private:
    RunRegistry* _pRuns = nullptr;              // Not owned; outlives this.
    SlotPool* _pSlots = nullptr;                // Not owned; outlives this.
    inference::LlmBackend* _pUtility = nullptr; // Not owned; outlives this. Used under _utilityMutex.
    std::mutex _utilityMutex = {};
    HealthInfo _health = {};
    Log _log = {};

public:
    // Throws ContractError for an empty log.
    Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log);

    // Handles one request: `method` ("GET", "POST", ...), `path` ("/v1/runs/save_1/open"), `body` (JSON text, or empty).
    // Never throws for the request's or the run's fault: every failure is its status and
    // {"api_version", "error": {"status", "message"}}; a 500 is logged too.
    ApiResponse Handle(std::string_view method, std::string_view path, std::string_view body);

    // An exception `what` that escaped the HTTP layer while it served `method` `path` (a bug): logged, and its 500
    // error body.
    nlohmann::json EscapedError(std::string_view method, std::string_view path, std::string_view what);
};

} // namespace morpheus::server
