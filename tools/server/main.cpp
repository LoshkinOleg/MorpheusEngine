// morpheus_server: the HTTP server for hosted runs (docs/superpowers/specs/2026-11-16-morpheus-server-design.md).
//   morpheus_server [--port N] [--runs DIR] [--settings FILE]
// It reads settings.json (which must name the llama-server backend), checks both llama-servers, and serves
// http://127.0.0.1:N until Ctrl+C or SIGTERM.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "core/Text.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"

using namespace morpheus;

namespace {

constexpr int DEFAULT_PORT = 4998;
constexpr const char* DEFAULT_RUNS = "runs";
constexpr const char* DEFAULT_SETTINGS = "settings.json";
constexpr const char* USAGE = "usage: morpheus_server [--port N] [--runs DIR] [--settings FILE]\n";
constexpr int MAX_PORT = 65535;
constexpr std::chrono::milliseconds SIGNAL_POLL = std::chrono::milliseconds(100);

// Set by the signal handler, polled by main. A lock-free atomic may be set from a handler.
std::atomic<bool> stopRequested = false;

extern "C" void OnSignal(int)
{
    stopRequested = true;
}

} // namespace

int main(int argc, char** argv)
{
    // The flags, each with a value.
    int port = DEFAULT_PORT;
    std::filesystem::path runsDir = DEFAULT_RUNS;
    std::filesystem::path settingsFile = DEFAULT_SETTINGS;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc || (flag != "--port" && flag != "--runs" && flag != "--settings")) {
            std::cerr << USAGE;
            return 2;
        }
        const std::string value = argv[++i];
        if (flag == "--port") {
            // Digits only, from 1 to 65535.
            std::size_t used = 0;
            try {
                port = std::stoi(value, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != value.size() || port < 1 || port > MAX_PORT) {
                std::cerr << "morpheus_server: --port needs a number from 1 to 65535\n" << USAGE;
                return 2;
            }
        } else if (flag == "--runs") {
            runsDir = value;
        } else {
            settingsFile = value;
        }
    }

    // One logger for every thread: a line at a time on stderr.
    std::mutex logMutex;
    const server::Log log = [&logMutex](const std::string& line) {
        std::scoped_lock lock(logMutex);
        std::cerr << "morpheus_server: " << line << '\n';
    };

    try {
        const core::Settings settings = core::LoadSettings(settingsFile);
        if (settings.backend != core::BackendKind::LlamaServer) {
            log(std::format("{}: the backend is \"{}\"; morpheus_server needs \"backend\": \"{}\"",
                            core::PathText(settingsFile), core::BackendKindName(settings.backend),
                            core::BackendKindName(core::BackendKind::LlamaServer)));
            return 1;
        }

        // Every backend talks to the same servers; a call left on the default slot goes to the upkeep slot.
        // settings.json's server slot is for Morpheus's own game: the server hands out slots itself.
        llamaserver::LlamaServerConfig config;
        config.chatUrl = settings.chatUrl;
        config.embeddingUrl = settings.embeddingUrl;
        config.slot = server::SlotPool::UPKEEP_SLOT;
        config.contextSize = settings.contextSize;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;

        // The utility backend checks both servers now, serves /v1/tokenize and /v1/embeddings, and tells the slot
        // count and the model ids.
        llamaserver::LlamaServerBackend utility(config);
        server::SlotPool slots(utility.SlotCount());
        if (utility.SlotCount() == 1) {
            log("the chat server has one slot: folding and turns share it, so their cached prompts evict each other "
                "(start llama-server with --parallel 3 or more)");
        }
        server::RunRegistry runs(runsDir, settings,
                                 [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); }, log);
        server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                        log);
        server::HttpServer http(api);
        http.Start(port);
        log(std::format("listening on http://127.0.0.1:{}; runs in '{}'; chat model {} ({} slots), embedding model {}",
                        http.Port(), core::PathText(std::filesystem::absolute(runsDir)), utility.ChatModelId(),
                        utility.SlotCount(), utility.EmbeddingModelId()));

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        while (!stopRequested) {
            std::this_thread::sleep_for(SIGNAL_POLL);
        }

        // Running turns and folds stop first, so the HTTP handlers waiting on them return; then the server stops,
        // and the workers join as `runs` goes.
        log("stopping");
        runs.StopAll();
        http.Stop();
        return 0;
    } catch (const std::exception& error) {
        log(error.what());
        return 1;
    }
}
