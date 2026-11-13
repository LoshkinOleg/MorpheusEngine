### Task 4: The app answers through the seat

`AppController` owns the seat and wraps every backend its factory makes, so the engine host's backends (rebuilt ones too) answer through the same seat. With the seat off the wrapper is transparent, so every existing app test still passes.

**Files:**
- Modify: `src/app/model/AppController.h`, `src/app/model/AppController.cpp`
- Test: `tests/app/AppControllerTests.cpp`

**Interfaces:**
- Consumes: `ModelSeat`, `SeatLabel`, `SeatMode`, `SeatRequest` (Task 2); `ModelSeatBackend` (Task 3); `test::SeatOnlyFor` (Task 2).
- Produces: `ModelSeat& AppController::Seat();` Every backend `EngineHost` holds is `ModelSeatBackend(factory(settings), seat)`.

- [ ] **Step 1: Write the failing tests**

In `tests/app/AppControllerTests.cpp`, replace

```cpp
#include "app/AppTestSupport.h"
#include "app/model/AppController.h"
```

with

```cpp
#include "app/AppTestSupport.h"
#include "app/SeatTestSupport.h"
#include "app/model/AppController.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "core/TurnRecord.h"
```

and append:

```cpp
TEST_CASE("A resolve reply typed at the model seat that is not JSON proposes nothing, as a model mistake does",
          "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_resolve");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You stay where you are.")));
    OpenNewRun(controller, "seat");
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    const app::SeatRequest request = *controller.Seat().Pending();
    CHECK(request.label == app::SeatLabel::Resolve);
    CHECK_FALSE(request.spec.grammar.empty());
    CHECK_FALSE(request.draft);
    REQUIRE(controller.Seat().Submit(request.id, "I would rather not.", ""));
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // The engine reads the reply as it reads a model's: unreadable, so nothing is proposed, and the turn goes on.
    REQUIRE(controller.Run().History().size() == 1);
    const core::TurnRecord& turn = controller.Run().History()[0];
    CHECK(turn.resolve.at("unparsed") == "I would rather not.");
    CHECK(turn.applied.empty());
    CHECK_FALSE(turn.timings.at("resolve").value("error", std::string()).empty());
    CHECK(turn.narration == "You stay where you are.");
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.Run().State().at("player").at("fields").at("location") == "shore");
}

TEST_CASE("Cancelling a turn while the model seat waits closes it with TurnCancelled", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_cancel");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder("You walk.")));
    OpenNewRun(controller, "seat");
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk slowly.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // Cancelled, not failed: no turn, no error, and the input comes back.
    CHECK(controller.Run().History().empty());
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.TakeRestoredInput() == std::optional<std::string>("I walk slowly."));
    CHECK_FALSE(controller.Seat().Pending());
}

TEST_CASE("The model seat keeps its switch and modes across a restart that reloads the models", "[app_controller]")
{
    const std::filesystem::path dir = test::SettingsFolder("app_seat_restart");
    std::atomic<int> made = 0;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder("You walk."));
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        ++made;
        return scripted(settings);
    });
    OpenNewRun(controller, "restarted");
    app::ModelSeat& seat = controller.Seat();
    test::SeatOnlyFor(seat, app::SeatLabel::Resolve, app::SeatMode::Human);
    seat.SetMode(app::SeatLabel::Narrate, app::SeatMode::Draft);

    // A GPU layers change reloads the models.
    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.gpuLayers = edit.gpuLayers == 7 ? 8 : 7;
    REQUIRE(controller.ApplySettings(edit).empty());
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    CHECK(made == 2);

    CHECK(&controller.Seat() == &seat);
    CHECK(seat.Enabled());
    CHECK(seat.ModeOf(app::SeatLabel::Resolve) == app::SeatMode::Human);
    CHECK(seat.ModeOf(app::SeatLabel::Narrate) == app::SeatMode::Draft);
    CHECK(seat.ModeOf(app::SeatLabel::Review) == app::SeatMode::Model);

    // The reloaded backend answers through the same seat.
    controller.Submit("I walk.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    CHECK(controller.Seat().Pending()->label == app::SeatLabel::Resolve);
    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `'Seat': is not a member of 'morpheus::app::AppController'`.

- [ ] **Step 3: Own the seat and wrap the factory**

In `src/app/model/AppController.h`, replace

```cpp
#include <filesystem>
#include <optional>
```

with

```cpp
#include <filesystem>
#include <memory>
#include <optional>
```

replace

```cpp
#include "app/model/EngineHost.h"
```

with

```cpp
#include "app/model/EngineHost.h"
#include "app/model/ModelSeat.h"
```

replace

```cpp
// The app apart from drawing (UI thread only): the worlds and their runs, the settings, the engine and the open run.
```

with

```cpp
// The app apart from drawing (UI thread only): the worlds and their runs, the settings, the engine, the open run and
// the model seat.
```

replace

```cpp
    AppPaths _paths = {};
    EngineHost _engine;
```

with

```cpp
    AppPaths _paths = {};
    // The model seat, shared with every backend the engine host builds. Declared before _engine, whose factory wraps
    // each backend around it: it exists first, and the backends' shares keep it alive as long as they live.
    std::shared_ptr<ModelSeat> _pSeat = std::make_shared<ModelSeat>();
    EngineHost _engine;
```

replace

```cpp
    // Loads the settings (DataError when they are invalid: the app cannot run without them), lists the worlds
    // (DataError when the worlds folder is missing) and starts loading the engine.
    AppController(AppPaths paths, EngineHost::BackendFactory factory);
```

with

```cpp
    // Loads the settings (DataError when they are invalid: the app cannot run without them), lists the worlds
    // (DataError when the worlds folder is missing) and starts loading the engine. Every backend `factory` makes is
    // wrapped in a ModelSeatBackend around the seat; with the seat off, the wrapper only forwards.
    AppController(AppPaths paths, EngineHost::BackendFactory factory);
```

and replace

```cpp
    const std::vector<WorldEntry>& Worlds() const;
```

with

```cpp
    const std::vector<WorldEntry>& Worlds() const;
    // The model seat: its switch, the modes, and the call waiting for a reply.
    ModelSeat& Seat();
```

In `src/app/model/AppController.cpp`, replace

```cpp
#include "core/Errors.h"
```

with

```cpp
#include "app/model/ModelSeatBackend.h"
#include "core/Errors.h"
```

replace

```cpp
AppController::AppController(AppPaths paths, EngineHost::BackendFactory factory)
    : _paths(std::move(paths))
    , _engine(std::move(factory))
{
```

with

```cpp
AppController::AppController(AppPaths paths, EngineHost::BackendFactory factory)
    : _paths(std::move(paths))
    , _engine([pSeat = _pSeat, factory = std::move(factory)](
                  const core::Settings& settings) -> std::unique_ptr<inference::LlmBackend> {
        // Every backend answers through the model seat, a rebuilt one too, so the switch and modes survive restarts.
        return std::make_unique<ModelSeatBackend>(factory(settings), pSeat);
    })
{
```

and replace

```cpp
const std::vector<WorldEntry>& AppController::Worlds() const
{
    return _worlds;
}
```

with

```cpp
const std::vector<WorldEntry>& AppController::Worlds() const
{
    return _worlds;
}

ModelSeat& AppController::Seat()
{
    return *_pSeat;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[app_controller],[engine_host],[ui]"`
Expected: all pass (the existing ones unchanged: the seat is off).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 306`.

- [ ] **Step 5: Commit**

```bash
git add src/app/model/AppController.h src/app/model/AppController.cpp tests/app/AppControllerTests.cpp
git commit -m "Let the app answer model calls through the model seat" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

