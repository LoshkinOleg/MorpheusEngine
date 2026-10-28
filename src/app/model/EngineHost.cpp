#include "app/model/EngineHost.h"

#include <exception>
#include <utility>

#include "core/Errors.h"

namespace morpheus::app {

EngineHost::EngineHost(BackendFactory factory)
    : _factory(std::move(factory))
{
}

void EngineHost::Start(core::Settings settings)
{
    MORPHEUS_REQUIRE(_status != Status::Loading, "EngineHost: Start while models load");
    // Whether the loaded backend was made for the same models, prefixes and context.
    const bool sameBackend = _pBackend && settings.chatModel == _settings.chatModel &&
                             settings.embeddingModel == _settings.embeddingModel &&
                             settings.embeddingQueryPrefix == _settings.embeddingQueryPrefix &&
                             settings.embeddingDocumentPrefix == _settings.embeddingDocumentPrefix &&
                             settings.contextSize == _settings.contextSize && settings.gpuLayers == _settings.gpuLayers;
    _pWorker.reset(); // Cancels its command and joins it; the run it had open closes.
    _settings = std::move(settings);
    _failure.clear();
    if (sameBackend) {
        _pWorker = std::make_unique<turn::Worker>(_settings, *_pBackend);
        _status = Status::Ready;
        return;
    }

    // Free the old models before loading new ones: both may not fit in GPU memory.
    _pBackend.reset();
    {
        std::scoped_lock lock(_loadMutex);
        _pLoaded.reset();
        _loadError.clear();
        _loadDone = false;
    }
    _status = Status::Loading;
    // The loader only calls the factory and hands its result over under the lock; Poll takes it.
    _loader = std::jthread([this, settings = _settings] {
        std::unique_ptr<inference::LlmBackend> pLoaded = nullptr;
        std::string error;
        try {
            pLoaded = _factory(settings);
            MORPHEUS_REQUIRE(pLoaded != nullptr, "EngineHost: the backend factory returned no backend");
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        std::scoped_lock lock(_loadMutex);
        _pLoaded = std::move(pLoaded);
        _loadError = std::move(error);
        _loadDone = true;
    });
}

void EngineHost::Poll()
{
    if (_status != Status::Loading) {
        return;
    }
    std::unique_ptr<inference::LlmBackend> pLoaded = nullptr;
    std::string error;
    {
        std::scoped_lock lock(_loadMutex);
        if (!_loadDone) {
            return; // Still loading.
        }
        pLoaded = std::move(_pLoaded);
        error = std::move(_loadError);
    }
    _loader.join(); // The loader has finished its work: this returns at once.
    if (!pLoaded) {
        _status = Status::Failed;
        _failure = std::move(error);
        return;
    }
    _pBackend = std::move(pLoaded);
    _pWorker = std::make_unique<turn::Worker>(_settings, *_pBackend);
    _status = Status::Ready;
}

EngineHost::Status EngineHost::CurrentStatus() const
{
    return _status;
}

const std::string& EngineHost::Failure() const
{
    return _failure;
}

const core::Settings& EngineHost::CurrentSettings() const
{
    return _settings;
}

turn::Worker* EngineHost::ActiveWorker()
{
    return _status == Status::Ready ? _pWorker.get() : nullptr;
}

} // namespace morpheus::app
