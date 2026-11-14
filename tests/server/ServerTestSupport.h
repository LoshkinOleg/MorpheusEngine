#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/Api.h"
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

} // namespace morpheus::test
