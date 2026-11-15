#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "fakes/FakeLlamaServer.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"
#include "llamaserver/ServerTestSupport.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/RunWorker.h"
#include "server/SlotPool.h"
#include "server/TurnStream.h"

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


// Every event of `stream` until it ends, waiting at most 10 s in all.
inline std::vector<server::StreamEvent> ReadAll(server::TurnStream& stream)
{
    std::vector<server::StreamEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stream.Ended() && std::chrono::steady_clock::now() < deadline) {
        if (std::optional<server::StreamEvent> event = stream.Next(std::chrono::milliseconds(50))) {
            events.push_back(std::move(*event));
        }
    }
    return events;
}

// An Api over runs in a fresh folder for test `name`: every run's backend is a scripted one answering with
// `responder` (pieceDelay before each streamed piece; backendDown makes new run backends fail to start), the utility
// backend counts and embeds as ScriptedBackend does, and the pool has 3 slots. Log lines are kept.
struct ApiHarness {
    std::filesystem::path runsDir = {};
    ScriptedBackend::Responder responder = {};
    std::chrono::milliseconds pieceDelay = std::chrono::milliseconds(0);
    std::atomic<bool> backendDown = false;
    std::mutex mutex = {};                     // Guards logLines and backends.
    std::vector<std::string> logLines = {};
    std::vector<ScriptedBackend*> backends = {}; // Each run's backend, in the order the runs opened.
    ScriptedBackend utility{16384};
    server::SlotPool slots{3};
    server::RunRegistry runs;
    server::Api api;

    ApiHarness(std::string_view name, ScriptedBackend::Responder responderIn)
        : runsDir(FreshTempDir(name) / "runs")
        , responder(std::move(responderIn))
        , runs(runsDir, HostedSettings(), [this] { return MakeBackend(); }, [this](const std::string& line) { Log(line); })
        , api(runs, slots, utility, {"chat-model", "embed-model", 3}, [this](const std::string& line) { Log(line); })
    {
    }

    std::unique_ptr<inference::LlmBackend> MakeBackend()
    {
        if (backendDown) {
            throw inference::InferenceError("cannot reach http://127.0.0.1:1/props");
        }
        auto pBackend = std::make_unique<ScriptedBackend>(16384, responder);
        pBackend->SetPieceDelay(pieceDelay);
        std::scoped_lock lock(mutex);
        backends.push_back(pBackend.get());
        return pBackend;
    }

    void Log(const std::string& line)
    {
        std::scoped_lock lock(mutex);
        logLines.push_back(line);
    }

    server::ApiResponse Post(const std::string& path, const nlohmann::json& body)
    {
        return api.Handle("POST", path, body.dump());
    }

    server::ApiResponse Get(const std::string& path)
    {
        return api.Handle("GET", path, "");
    }
};

// A backend config on fake servers `chat` and `embed` for a whole server: calls left at -1 go to the upkeep slot, and
// the context is HostedSettings' 16384 (the fakes hold 16384 per slot).
inline llamaserver::LlamaServerConfig ServerConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)
{
    llamaserver::LlamaServerConfig config = ConfigFor(chat, embed);
    config.slot = server::SlotPool::UPKEEP_SLOT;
    config.contextSize = 16384;
    return config;
}

// `text` in pieces of 4 bytes, as a fake server streams a completion. The fixtures' texts are ASCII.
inline std::vector<std::string> Pieces(const std::string& text)
{
    std::vector<std::string> pieces;
    for (std::size_t at = 0; at < text.size(); at += 4) {
        pieces.push_back(text.substr(at, 4));
    }
    return pieces;
}

// The whole server over real sockets, with fake llama-servers behind it, for test `name`: each run's backend and the
// utility backend on the fakes, HostedSettings, and the server on a free port of 127.0.0.1. The chat fake refuses
// prompts without a user message, as llama-server does with Qwen3.5, the host's model. Log lines are dropped.
struct HttpHarness {
    FakeLlamaServer chat;
    FakeLlamaServer embed;
    std::filesystem::path runsDir = {};
    llamaserver::LlamaServerBackend utility;
    server::SlotPool slots;
    server::RunRegistry runs;
    server::Api api;
    server::HttpServer http;

    explicit HttpHarness(std::string_view name)
        : runsDir(FreshTempDir(name) / "runs")
        , utility(ServerConfigFor(chat, embed))
        , slots(utility.SlotCount())
        , runs(runsDir, HostedSettings(),
               [this] { return std::make_unique<llamaserver::LlamaServerBackend>(ServerConfigFor(chat, embed)); },
               [](const std::string&) {})
        , api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
              [](const std::string&) {})
        , http(api)
    {
        chat.RefuseWithoutUserMessage(true);
        http.Start(0);
    }

    // Running turns stop first, so the handlers waiting on them return; then the server stops.
    ~HttpHarness()
    {
        runs.StopAll();
        http.Stop();
    }

    httplib::Client Client() const
    {
        return httplib::Client("127.0.0.1", http.Port());
    }
};

// A JSON answer over HTTP: its status, its Content-Type and its body.
struct HttpJson {
    int status = 0;
    std::string contentType = {};
    nlohmann::json body = nullptr;
};

inline HttpJson JsonPost(httplib::Client& client, const std::string& path, const nlohmann::json& body)
{
    const httplib::Result result = client.Post(path, body.dump(), "application/json");
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

inline HttpJson JsonGet(httplib::Client& client, const std::string& path)
{
    const httplib::Result result = client.Get(path);
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

// A turn over HTTP: the status and Content-Type, the stream's events parsed back ("event:" and "data:" lines), or
// the JSON body of a refused turn.
struct HttpTurn {
    int status = 0;
    std::string contentType = {};
    std::vector<server::StreamEvent> events = {};
    nlohmann::json body = nullptr;
};

// Posts a turn and reads its answer. `keepReading`, when given, sees each event as it arrives; returning false
// closes the connection there (a client that leaves mid-stream; the status is then 0).
inline HttpTurn PostTurn(httplib::Client& client, const std::string& path, const nlohmann::json& body,
                         const std::function<bool(const server::StreamEvent&)>& keepReading = {})
{
    HttpTurn turn;
    std::string raw;     // Every byte received.
    std::string pending; // Bytes not yet parsed into events.
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.body = body.dump();
    request.set_header("Content-Type", "application/json");
    request.content_receiver = [&](const char* pData, std::size_t length, std::size_t, std::size_t) {
        raw.append(pData, length);
        pending.append(pData, length);
        // Each event ends with a blank line.
        for (std::size_t end = pending.find("\n\n"); end != std::string::npos; end = pending.find("\n\n")) {
            std::istringstream lines(pending.substr(0, end));
            pending.erase(0, end + 2);
            server::StreamEvent event;
            for (std::string line; std::getline(lines, line);) {
                if (line.starts_with("event: ")) {
                    event.name = line.substr(7);
                } else if (line.starts_with("data: ")) {
                    event.data = nlohmann::json::parse(line.substr(6));
                }
            }
            turn.events.push_back(event);
            if (keepReading && !keepReading(event)) {
                return false;
            }
        }
        return true;
    };
    const httplib::Result result = client.send(request);
    if (result) {
        turn.status = result->status;
        turn.contentType = result->get_header_value("Content-Type");
        // cpp-httplib may hand an error answer's body to the receiver or keep it in the response.
        const std::string& text = raw.empty() ? result->body : raw;
        if (turn.events.empty() && !text.empty()) {
            turn.body = nlohmann::json::parse(text, nullptr, false);
        }
    }
    return turn;
}

} // namespace morpheus::test
