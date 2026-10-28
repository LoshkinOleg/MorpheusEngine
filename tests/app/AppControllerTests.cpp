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
