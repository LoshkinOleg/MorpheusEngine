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
