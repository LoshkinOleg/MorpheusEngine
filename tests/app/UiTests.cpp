#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/model/AppController.h"
#include "app/model/SettingsEdit.h"
#include "app/ui/Ui.h"
#include "core/MemoryMode.h"
#include "inference/Llm.h"

using namespace morpheus;

namespace {

const std::string MOVE =
    R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";
const std::string NARRATION = "You walk along the shore to the lighthouse, where a lamp burns in the high window "
                              "and gulls circle the tower.";

// Resolve calls (their grammar asks for an intent) get MOVE; narrate calls get NARRATION.
test::ScriptedBackend::Responder Responder()
{
    return [](const inference::Prompt&, const inference::GenerationSpec& spec) {
        return spec.grammar.find("intent") != std::string::npos ? MOVE : NARRATION;
    };
}

// An assisted-mode turn: the lookup asks for lore about the keeper, and the resolve output has a check with both
// branches, a note, and a change the engine rejects.
test::ScriptedBackend::Responder AssistedResponder()
{
    return [](const inference::Prompt&, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return R"({"lookups":[{"source":"lore","query":"keeper"}]})";
        }
        if (spec.grammar.find("intent") != std::string::npos) {
            return R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall",)"
                   R"("on_success":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}],)"
                   R"("on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],)"
                   R"("changes":[{"op":"set","path":"player.wings","value":true,"reason":"flies"}],)"
                   R"("notes":["The cliff is slick."]})";
        }
        return "You climb.";
    };
}

// Draws a few frames of the whole UI, taking the worker's events before each as the app does. Several frames let
// windows appear, dock and lay out.
void DrawFrames(test::HeadlessImGui& imgui, app::AppController& controller, app::Ui& ui)
{
    for (int frame = 0; frame < 3; ++frame) {
        controller.Frame();
        imgui.Frame([&] { ui.Draw(controller); });
    }
}

} // namespace

TEST_CASE("The UI draws every stage of a run", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_run");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder(), std::chrono::milliseconds(10)));
    app::Ui ui;

    DrawFrames(imgui, controller, ui); // Models loading, no run.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawFrames(imgui, controller, ui); // Ready, no run.

    controller.NewRun(controller.Worlds().at(0), "ui", 7);
    DrawFrames(imgui, controller, ui); // Opening.
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] {
        return controller.Run().Pending() && !controller.Run().Pending()->narration.empty();
    });
    DrawFrames(imgui, controller, ui); // Mid-turn: resolved, narration streaming.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawFrames(imgui, controller, ui); // One committed turn.

    controller.Regenerate();
    DrawFrames(imgui, controller, ui); // The latest turn hidden while it plays again.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    controller.Submit("   "); // An empty input: the turn fails with a message for the player.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    CHECK_FALSE(controller.Run().LastError().empty());
    DrawFrames(imgui, controller, ui);

    controller.Undo();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawFrames(imgui, controller, ui);
    CHECK(controller.Run().History().empty());
}

TEST_CASE("The UI draws an engine that failed to load", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_failed");
    app::AppController controller(test::PathsIn(dir),
                                  [](const core::Settings&) -> std::unique_ptr<inference::LlmBackend> {
                                      throw inference::InferenceError("cannot load model 'chat.gguf'");
                                  });
    app::Ui ui;
    test::PumpUntil(controller,
                    [&] { return controller.Engine().CurrentStatus() == app::EngineHost::Status::Failed; });
    DrawFrames(imgui, controller, ui);
    CHECK_FALSE(controller.CanPost());
}

TEST_CASE("The inspectors draw a run with checks, notes, lookups and retrieval", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_inspectors");
    app::SettingsEdit edit = app::ReadSettingsEdit(dir / "settings.json");
    edit.memoryMode = core::MemoryMode::Assisted;
    app::SaveSettingsEdit(dir / "settings.json", edit);
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(AssistedResponder()));
    app::Ui ui;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "inspect", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    for (const char* input : {"I climb the cliff.", "I climb again."}) {
        controller.Submit(input);
        test::PumpUntil(controller, [&] { return controller.CanPost(); });
    }
    REQUIRE(controller.Run().History().size() == 2);
    CHECK_FALSE(controller.Run().Memory().notes.empty());
    CHECK_FALSE(controller.Run().History().back().rejected.empty());
    DrawFrames(imgui, controller, ui);

    // The Context window's raw view, which the default frames do not show.
    app::ContextWindow context;
    context.showRaw = true;
    imgui.Frame([&] { context.Draw(controller); });
}
