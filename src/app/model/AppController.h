#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/model/Catalog.h"
#include "app/model/EngineHost.h"
#include "app/model/RunModel.h"
#include "app/model/SettingsEdit.h"
#include "turn/Worker.h"

namespace morpheus::app {

// Where the app finds its files.
struct AppPaths {
    std::filesystem::path settingsFile = {};
    std::filesystem::path projectsDir = {}; // World folders.
    std::filesystem::path savesDir = {};    // Runs: <world id>/<name>.db.
};

// The app apart from drawing (UI thread only): the worlds and their runs, the settings, the engine and the open run.
// Windows read it and call its actions. An action the UI does not offer at the moment is a ContractError: commands
// need CanPost(), and ApplySettings needs CanApplySettings().
class AppController {
private:
    AppPaths _paths = {};
    EngineHost _engine;
    RunModel _run = {};
    std::vector<WorldEntry> _worlds = {};
    std::optional<turn::LoadRun> _opening = {}; // The run being opened, until RunOpened, Error or TurnCancelled.
    std::optional<turn::LoadRun> _openRun = {}; // The open run's files, to reopen it after a restart.
    std::optional<turn::LoadRun> _reopen = {};  // Posted once the restarted engine is Ready.

public:
    // The app's files under `root`: settings.json, game_projects/ and saves/.
    static AppPaths PathsUnder(const std::filesystem::path& root);

    // Loads the settings (DataError when they are invalid: the app cannot run without them), lists the worlds
    // (DataError when the worlds folder is missing) and starts loading the engine.
    AppController(AppPaths paths, EngineHost::BackendFactory factory);

    // Takes a finished engine load, reopens a run after a restart, and applies the worker's events. Call once per
    // frame, before drawing.
    void Frame();

    const AppPaths& Paths() const;
    const EngineHost& Engine() const;
    const RunModel& Run() const;
    const std::vector<WorldEntry>& Worlds() const;
    // The open run's world folder and run file, when a run is open.
    const std::optional<turn::LoadRun>& OpenRun() const;

    // Whether a command can be posted now: the engine is Ready, no command runs and no reopen waits.
    bool CanPost() const;
    // Whether ApplySettings is allowed now: no command runs and no models load.
    bool CanApplySettings() const;

    // Lists the worlds again, after world files were added or edited.
    void RefreshWorlds();

    // Starts a new run of `world` named `name` (a loadable world, and RunNameProblem must be empty). A nullopt seed
    // picks one at random.
    void NewRun(const WorldEntry& world, std::string_view name, std::optional<std::uint64_t> seed);
    void ContinueRun(const WorldEntry& world, const RunEntry& run);
    void Submit(std::string text);
    void Undo();
    void Regenerate();
    // Cancels the running turn or regenerate. Does nothing when no command runs.
    void Cancel();

    // Saves `edit` to the settings file and restarts the engine with it; the open run reopens once the engine is
    // Ready. Returns why the edit was refused (the settings' DataError: nothing changed), or "" when applied.
    std::string ApplySettings(const SettingsEdit& edit);

    // The input of a turn that was cancelled or failed, once.
    std::optional<std::string> TakeRestoredInput();

private:
    // Records `command` in the run model and posts it to the worker.
    void Post(turn::Command command);
};

} // namespace morpheus::app
