#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "SceneWorld.h"
#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/model/AppController.h"
#include "app/model/Inspect.h"
#include "app/model/SettingsEdit.h"
#include "app/ui/Ui.h"
#include "core/MemoryMode.h"
#include "inference/Llm.h"
#include "turn/EngineText.h"

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

const std::string RENARRATION = "You walk the last steps along the shore to the lighthouse door.";

// Resolve calls get MOVE. Each turn's first review finds a contradiction and its second none (the count is shared
// by every backend the factory makes). The first narration is NARRATION, the renarrate (its message carries the
// correction) RENARRATION.
test::ScriptedBackend::Responder RenarrateResponder()
{
    auto pReviews = std::make_shared<int>(0);
    return [pReviews](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (spec.grammar.find("intent") != std::string::npos) {
            return MOVE;
        }
        if (prompt.messages.front().content == turn::REVIEW_RULES) {
            return (*pReviews)++ % 2 == 0
                       ? R"({"consistent":false,"problems":["The player never reaches the lighthouse."],"digest":"The player set off."})"
                       : R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";
        }
        const bool renarrate = prompt.messages.back().content.find(turn::RENARRATE_CORRECTION) != std::string::npos;
        return renarrate ? RENARRATION : NARRATION;
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

TEST_CASE("The inspectors draw a renarrated turn and scene retrieval", "[ui]")
{
    test::HeadlessImGui imgui;
    // settings.example.json turns the narration check and the compact transcript on. The only world is the mini
    // world with a scene, whose player starts on the shore.
    const std::filesystem::path dir = test::SettingsFolder("ui_review");
    app::AppPaths paths = test::PathsIn(dir);
    paths.projectsDir = test::SceneWorldDir("ui_review_worlds").parent_path();
    app::AppController controller(paths, test::ScriptedFactory(RenarrateResponder()));
    app::Ui ui;

    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "review", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    controller.Submit("I look around.");
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    REQUIRE(controller.Run().History().size() == 1);
    const core::TurnRecord& turn = controller.Run().History().back();
    CHECK(turn.narration == RENARRATION);
    CHECK(app::ReplacedNarration(turn.timings) == NARRATION);
    CHECK(app::ReviewRows(turn.timings).size() == 2);
    CHECK(app::SceneValues(turn.timings) == std::vector<std::string>{"shore"});
    const std::vector<app::RetrievalRow> rows =
        app::RetrievalRows(turn.timings, controller.Run().History(), controller.Run().Memory().notes);
    const bool sceneHit = std::any_of(rows.begin(), rows.end(), [](const app::RetrievalRow& row) {
        return row.ref == "shore" && std::find(row.reasons.begin(), row.reasons.end(), "scene") != row.reasons.end();
    });
    CHECK(sceneHit);
    DrawFrames(imgui, controller, ui); // The Turn log with both reviews and the replaced narration; the Memory window
                                       // with the reasons and the scene; the Settings window with the new keys.
}
