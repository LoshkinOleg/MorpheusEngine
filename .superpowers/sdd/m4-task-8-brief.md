### Task 8: The app controller

`AppController` is the app apart from drawing. It holds the paths, the catalog, the engine and the run model. It posts commands, applies the worker's events each frame, and keeps track of which files the open run lives in. That way a settings restart can reopen the run on the new worker. The windows (Tasks 9 and 10) only read it and call its actions.

**Files:**
- Create: `src/app/model/AppController.h`, `src/app/model/AppController.cpp`, `tests/app/AppControllerTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `model/AppController.cpp`), `tests/app/AppTestSupport.h` (add `PathsIn`, `ScriptedFactory`, `PumpUntil`)

**Interfaces:**
- Consumes: Tasks 3 to 7.
- Produces:
  - `struct app::AppPaths { std::filesystem::path settingsFile, projectsDir, savesDir; }`.
  - `class app::AppController` with:
    - `static AppPaths PathsUnder(const std::filesystem::path& root)` and `AppController(AppPaths, EngineHost::BackendFactory)`;
    - `void Frame()`, `const AppPaths& Paths() const`, `const EngineHost& Engine() const`, `const RunModel& Run() const` and `const std::vector<WorldEntry>& Worlds() const`;
    - `const std::optional<turn::LoadRun>& OpenRun() const`, `bool CanPost() const`, `bool CanApplySettings() const` and `void RefreshWorlds()`;
    - `void NewRun(const WorldEntry&, std::string_view name, std::optional<std::uint64_t> seed)` and `void ContinueRun(const WorldEntry&, const RunEntry&)`;
    - `void Submit(std::string)`, `void Undo()`, `void Regenerate()` and `void Cancel()`;
    - `std::string ApplySettings(const SettingsEdit&)` and `std::optional<std::string> TakeRestoredInput()`.
  - Test support: `test::PathsIn(dir)`, `test::ScriptedFactory(responder, pieceDelay)` and `test::PumpUntil(controller, done)`.

- [ ] **Step 1: Write the test support and the failing tests**

Add to `tests/app/AppTestSupport.h`. Include `<chrono>`, `<functional>`, `<memory>`, `<thread>`, `<catch2/catch_test_macros.hpp>`, `"app/model/AppController.h"` and `"fakes/ScriptedBackend.h"`.

```cpp
// App paths for a test: the settings in `dir`, the test worlds, and saves in `dir`.
inline app::AppPaths PathsIn(const std::filesystem::path& dir)
{
    return {dir / "settings.json", TestDataDir() / "worlds", dir / "saves"};
}

// A factory for ScriptedBackends that answer with `responder`, sized by the settings' context. A piece delay slows
// streaming, so a test can catch a turn midway.
inline app::EngineHost::BackendFactory ScriptedFactory(ScriptedBackend::Responder responder,
                                                       std::chrono::milliseconds pieceDelay = {})
{
    return [responder, pieceDelay](const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        auto pBackend = std::make_unique<ScriptedBackend>(settings.contextSize, responder);
        pBackend->SetPieceDelay(pieceDelay);
        return pBackend;
    };
}

// Runs the controller's frames until `done` holds. Fails the test after 10 s.
template <typename Done>
void PumpUntil(app::AppController& controller, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        controller.Frame();
        if (done()) {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

Create `tests/app/AppControllerTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "TestPaths.h"
#include "app/AppTestSupport.h"
#include "app/model/AppController.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string MOVE =
    R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";

// Resolve calls (their grammar asks for an intent) get MOVE; narrate calls get `narration`.
test::ScriptedBackend::Responder Responder(std::string narration)
{
    return [narration](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.find("intent") != std::string::npos ? MOVE : narration;
    };
}

// A controller with the engine ready and a new run of the mini world named `name` open.
void OpenNewRun(app::AppController& controller, const std::string& name)
{
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), name, 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
}

} // namespace

TEST_CASE("AppController starts the engine, lists the worlds, and plays a run", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_play");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk to the lighthouse.")));
    CHECK_FALSE(controller.CanPost()); // The models load first.
    REQUIRE(controller.Worlds().size() == 1);
    CHECK(controller.Worlds()[0].id == "mini");

    OpenNewRun(controller, "first");
    CHECK(controller.Run().Meta().rngSeed == 7);
    REQUIRE(controller.OpenRun());
    CHECK(controller.OpenRun()->runFile == app::RunFile(dir / "saves", "mini", "first"));
    CHECK(app::ListRuns(dir / "saves", "mini").size() == 1);

    controller.Submit("I walk to the lighthouse.");
    CHECK_FALSE(controller.CanPost()); // One command at a time.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    REQUIRE(controller.Run().History().size() == 1);
    CHECK(controller.Run().History()[0].narration == "You walk to the lighthouse.");
    CHECK(controller.Run().State().at("player").at("fields").at("location") == "lighthouse");

    controller.Regenerate();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(controller.Run().History()[0].attempt == 1);

    controller.Undo();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(controller.Run().History().empty());
}

TEST_CASE("AppController continues a saved run", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_continue");
    {
        app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk.")));
        OpenNewRun(controller, "saved");
        controller.Submit("I walk to the lighthouse.");
        test::PumpUntil(controller, [&] { return controller.CanPost(); });
    } // Closing the app closes the run.

    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk.")));
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    const std::vector<app::RunEntry> runs = app::ListRuns(dir / "saves", "mini");
    REQUIRE(runs.size() == 1);
    controller.ContinueRun(controller.Worlds().at(0), runs[0]);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    CHECK(controller.Run().History().size() == 1);
}

TEST_CASE("Applying settings restarts the engine and reopens the run", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_apply");
    std::atomic<int> made = 0;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder("You walk."));
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        ++made;
        return scripted(settings);
    });
    OpenNewRun(controller, "tuned");
    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.narrateSampling.temperature = 1.2;
    REQUIRE(controller.CanApplySettings());
    CHECK(controller.ApplySettings(edit).empty());
    CHECK_FALSE(controller.Run().Open()); // Closed with the old worker...
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); }); // ...and reopened.
    CHECK(controller.Run().History().size() == 1);
    CHECK(made == 1); // A sampling change keeps the loaded models.
    CHECK(controller.Engine().CurrentSettings().narrateSampling.temperature == 1.2);
    CHECK(core::LoadSettings(dir / "settings.json").narrateSampling.temperature == 1.2);
}

TEST_CASE("Invalid settings are refused without touching the engine or the run", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_apply_invalid");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk.")));
    OpenNewRun(controller, "kept");

    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.chatModel = "models/missing.gguf";
    CHECK_THAT(controller.ApplySettings(edit), ContainsSubstring("does not exist"));
    CHECK(controller.CanPost());
    CHECK(controller.Run().Open());
}

TEST_CASE("A cancelled turn offers its input again", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_cancel");
    // 400 bytes stream as 100 pieces of 20 ms: the turn is still narrating when it is cancelled.
    app::AppController controller(test::PathsIn(dir),
                                  test::ScriptedFactory(Responder(std::string(400, 'w')), std::chrono::milliseconds(20)));
    OpenNewRun(controller, "cancelled");

    controller.Submit("I walk slowly.");
    test::PumpUntil(controller, [&] {
        return controller.Run().Pending() && !controller.Run().Pending()->narration.empty();
    });
    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK(controller.Run().History().empty());
    CHECK(controller.TakeRestoredInput() == std::optional<std::string>("I walk slowly."));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `model/AppController.cpp` to `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/model/AppController.h` is missing.

- [ ] **Step 3: Write the header**

Create `src/app/model/AppController.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `src/app/model/AppController.cpp`:

```cpp
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
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[app_controller]"`
Expected: all five tests pass. Then `ctest --preset tests`: everything passes.

- [ ] **Step 6: Commit**

```bash
git add src/app/CMakeLists.txt src/app/model/AppController.h src/app/model/AppController.cpp tests/app/AppTestSupport.h tests/app/AppControllerTests.cpp
git commit -m "Add the app controller over the catalog, settings, engine and run" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
