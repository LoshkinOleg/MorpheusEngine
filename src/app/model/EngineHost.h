#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "turn/Worker.h"

namespace morpheus::app {

// Owns the backend and the worker (UI thread only). Models load on a background thread, so the UI keeps drawing.
// A restart that changes no backend setting keeps the loaded backend and replaces only the worker.
class EngineHost {
public:
    // Makes the backend for `settings`. It may take seconds (it loads models) and may throw.
    using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>(const core::Settings&)>;

    enum class Status { Stopped, Loading, Ready, Failed };

private:
    BackendFactory _factory = {};
    core::Settings _settings = {};
    Status _status = Status::Stopped;
    std::string _failure = {};
    std::unique_ptr<inference::LlmBackend> _pBackend = nullptr;
    std::unique_ptr<turn::Worker> _pWorker = nullptr; // After the backend, so it is destroyed first.
    std::mutex _loadMutex = {};
    std::unique_ptr<inference::LlmBackend> _pLoaded = nullptr; // The loader's result, under _loadMutex.
    std::string _loadError = {};                               // The loader's failure, under _loadMutex.
    bool _loadDone = false;                                    // Under _loadMutex.
    std::jthread _loader = {}; // Last: joined first on destruction, before the members it writes.

public:
    explicit EngineHost(BackendFactory factory);

    // Starts the engine with `settings`. The running worker stops first, cancelling its command and closing its
    // run. The backend is kept when the model paths, embedding prefixes, context size and GPU layers are unchanged,
    // and the engine is Ready at once. Otherwise the backend is released (freeing its memory) and a new one loads in
    // the background. Throws ContractError while Loading: a load cannot be interrupted.
    void Start(core::Settings settings);

    // Takes a finished load: Ready with a new worker, or Failed. Call once per frame.
    void Poll();

    Status CurrentStatus() const;
    const std::string& Failure() const;             // Why the last load failed (Failed).
    const core::Settings& CurrentSettings() const;  // The settings of the last Start.
    turn::Worker* ActiveWorker();                   // The worker while Ready, else nullptr.
};

} // namespace morpheus::app
