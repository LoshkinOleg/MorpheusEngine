#include "app/model/AppController.h"

#include <random>
#include <utility>
#include <variant>

#include "core/Errors.h"

namespace morpheus::app {

AppPaths AppController::PathsUnder(const std::filesystem::path& root)
{
    return {root / "settings.json", root / "game_projects", root / "saves"};
}

AppController::AppController(AppPaths paths, EngineHost::BackendFactory factory)
    : _paths(std::move(paths))
    , _engine(std::move(factory))
{
    // Invalid settings stop the app here, naming the file and key.
    _engine.Start(core::LoadSettings(_paths.settingsFile));
    _worlds = ListWorlds(_paths.projectsDir);
}

void AppController::Frame()
{
    _engine.Poll();
    turn::Worker* pWorker = _engine.ActiveWorker();
    if (!pWorker) {
        return; // Loading, failed or stopped: no events to take.
    }
    // A run that was open before a settings restart reopens on the new worker.
    if (_reopen && _run.CurrentBusy() == RunModel::Busy::None) {
        turn::LoadRun reopen = std::move(*_reopen);
        _reopen.reset();
        _opening = reopen;
        Post(std::move(reopen));
    }
    for (const turn::Event& event : pWorker->PollEvents()) {
        _run.Apply(event);
        // Keep track of which files the open run lives in.
        if (!_opening) {
            continue;
        }
        if (std::holds_alternative<turn::RunOpened>(event)) {
            _openRun = std::move(_opening);
            _opening.reset();
        } else if (std::holds_alternative<turn::Error>(event)) {
            _openRun.reset(); // A failed open has already closed the previous run.
            _opening.reset();
        } else if (std::holds_alternative<turn::TurnCancelled>(event)) {
            _opening.reset(); // Cancelled before it ran: the previous run stays open.
        }
    }
}

const AppPaths& AppController::Paths() const
{
    return _paths;
}

const EngineHost& AppController::Engine() const
{
    return _engine;
}

const RunModel& AppController::Run() const
{
    return _run;
}

const std::vector<WorldEntry>& AppController::Worlds() const
{
    return _worlds;
}

const std::optional<turn::LoadRun>& AppController::OpenRun() const
{
    return _openRun;
}

bool AppController::CanPost() const
{
    return _engine.CurrentStatus() == EngineHost::Status::Ready && _run.CurrentBusy() == RunModel::Busy::None &&
           !_reopen;
}

bool AppController::CanApplySettings() const
{
    return _run.CurrentBusy() == RunModel::Busy::None && _engine.CurrentStatus() != EngineHost::Status::Loading;
}

void AppController::RefreshWorlds()
{
    _worlds = ListWorlds(_paths.projectsDir);
}

void AppController::NewRun(const WorldEntry& world, std::string_view name, std::optional<std::uint64_t> seed)
{
    MORPHEUS_REQUIRE(CanPost(), "AppController: NewRun while the UI cannot post");
    MORPHEUS_REQUIRE(world.error.empty() && RunNameProblem(_paths.savesDir, world.id, name).empty(),
                     "AppController: NewRun needs a loadable world and a usable run name");
    if (!seed) {
        std::random_device entropy;
        seed = (static_cast<std::uint64_t>(entropy()) << 32) | static_cast<std::uint64_t>(entropy());
    }
    const std::filesystem::path file = RunFile(_paths.savesDir, world.id, name);
    _opening = turn::LoadRun{world.folder, file};
    Post(turn::StartRun{world.folder, file, *seed});
}

void AppController::ContinueRun(const WorldEntry& world, const RunEntry& run)
{
    MORPHEUS_REQUIRE(CanPost(), "AppController: ContinueRun while the UI cannot post");
    _opening = turn::LoadRun{world.folder, run.file};
    Post(turn::LoadRun{world.folder, run.file});
}

void AppController::Submit(std::string text)
{
    MORPHEUS_REQUIRE(CanPost(), "AppController: Submit while the UI cannot post");
    Post(turn::SubmitInput{std::move(text)});
}

void AppController::Undo()
{
    MORPHEUS_REQUIRE(CanPost(), "AppController: Undo while the UI cannot post");
    Post(turn::UndoTurn{});
}

void AppController::Regenerate()
{
    MORPHEUS_REQUIRE(CanPost(), "AppController: Regenerate while the UI cannot post");
    Post(turn::RegenerateTurn{});
}

void AppController::Cancel()
{
    turn::Worker* pWorker = _engine.ActiveWorker();
    if (pWorker && _run.CurrentBusy() != RunModel::Busy::None) {
        pWorker->Cancel(); // The command still closes with its own event, which Frame applies.
    }
}

std::string AppController::ApplySettings(const SettingsEdit& edit)
{
    MORPHEUS_REQUIRE(CanApplySettings(), "AppController: ApplySettings while a command runs or models load");
    core::Settings settings;
    try {
        settings = SaveSettingsEdit(_paths.settingsFile, edit);
    } catch (const core::DataError& error) {
        return error.what(); // The window shows it; the file and the engine are unchanged.
    }
    // The restart closes the open run with the old worker; it reopens on the new one. A reopen still waiting from an
    // earlier restart (whose engine failed to load) is kept.
    if (_openRun) {
        _reopen = std::move(_openRun);
        _openRun.reset();
    }
    _run.Close();
    _engine.Start(std::move(settings));
    return {};
}

std::optional<std::string> AppController::TakeRestoredInput()
{
    return _run.TakeRestoredInput();
}

void AppController::Post(turn::Command command)
{
    turn::Worker* pWorker = _engine.ActiveWorker();
    MORPHEUS_REQUIRE(pWorker != nullptr, "AppController: no worker to post to");
    _run.OnPosted(command);
    pWorker->Post(std::move(command));
}

} // namespace morpheus::app
