#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName and ActivateItemByID: pressing the window's buttons.
#include <nlohmann/json.hpp>

#include "SceneWorld.h"
#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/SeatTestSupport.h"
#include "app/model/AppController.h"
#include "app/model/Inspect.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "app/ui/Ui.h"
#include "app/ui/Windows.h"
#include "fakes/ScriptedBackend.h"
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

// Draws the Model seat window alone, at a place and size that show its buttons, for `frames` frames, taking the
// worker's events before each as the app does. (Undocked: no Ui is drawn in these tests' contexts.)
void DrawSeatWindow(test::HeadlessImGui& imgui, app::AppController& controller, app::ModelSeatWindow& window,
                    int frames)
{
    for (int frame = 0; frame < frames; ++frame) {
        controller.Frame();
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1200.0f, 880.0f));
            window.Draw(controller);
        });
    }
}

// Presses the Model seat window's item `label` on the next frame, as a click would: ImGui's programmatic activation.
// Call between frames.
void PressSeatItem(const char* label)
{
    ImGuiWindow* pWindow = ImGui::FindWindowByName(app::MODEL_SEAT_WINDOW);
    REQUIRE(pWindow != nullptr);
    ImGui::ActivateItemByID(pWindow->GetID(label));
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

TEST_CASE("The Model seat window draws with no call waiting, and its checkbox turns the seat on", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_idle");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;

    DrawSeatWindow(imgui, controller, window, 3); // Models loading, the seat off.
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawSeatWindow(imgui, controller, window, 2);
    CHECK_FALSE(controller.Seat().Enabled());

    PressSeatItem("Answer as the model");
    DrawSeatWindow(imgui, controller, window, 2); // On, nothing waits.
    CHECK(controller.Seat().Enabled());
    CHECK_FALSE(controller.Seat().Pending());
}

TEST_CASE("Submit in the Model seat window answers a Human call with the reply box's text", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_human");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawSeatWindow(imgui, controller, window, 2); // The call, its limits and grammar, empty boxes, the prompt.
    CHECK(controller.Seat().Pending());           // Drawing answers nothing.

    PressSeatItem("Submit");
    DrawSeatWindow(imgui, controller, window, 1);
    CHECK_FALSE(controller.Seat().Pending());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    // The empty reply box reached the engine as the model's resolve output: unreadable, nothing proposed.
    REQUIRE(controller.Run().History().size() == 1);
    CHECK(controller.Run().History()[0].resolve.at("unparsed") == "");
    CHECK(controller.Run().History()[0].applied.empty());
    CHECK(controller.Run().History()[0].narration == NARRATION);
}

TEST_CASE("The Model seat window shows a Draft call's draft, checks it and submits it", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_draft");
    // The factory hands out the ScriptedBackend it made, to see the grammar checks reach it.
    std::atomic<test::ScriptedBackend*> pMade = nullptr;
    const app::EngineHost::BackendFactory scripted = test::ScriptedFactory(Responder());
    app::AppController controller(test::PathsIn(dir), [&](const core::Settings& settings) {
        std::unique_ptr<inference::LlmBackend> pBackend = scripted(settings);
        pMade = static_cast<test::ScriptedBackend*>(pBackend.get());
        return pBackend;
    });
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Draft);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    const app::SeatRequest request = *controller.Seat().Pending();
    REQUIRE(request.draft);
    CHECK(request.draft->text == MOVE);
    DrawSeatWindow(imgui, controller, window, 2); // The draft's thinking, the grammar, the prefilled boxes.

    // Check: the reply's token count and the grammar's verdict, from the real backend. It answers nothing.
    PressSeatItem("Check");
    DrawSeatWindow(imgui, controller, window, 2);
    REQUIRE(pMade.load() != nullptr);
    REQUIRE(pMade.load()->GrammarChecks().size() == 1);
    CHECK(pMade.load()->GrammarChecks()[0].text == MOVE);
    CHECK(controller.Seat().Pending());

    // Submit sends the draft unchanged.
    PressSeatItem("Submit");
    DrawSeatWindow(imgui, controller, window, 1);
    CHECK_FALSE(controller.Seat().Pending());
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    REQUIRE(controller.Run().History().size() == 1);
    CHECK(controller.Run().History()[0].resolve == nlohmann::json::parse(MOVE));
    CHECK(controller.Run().State().at("player").at("fields").at("location") == "lighthouse");
}

TEST_CASE("The UI draws the status bar and the docked windows while the model seat waits", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_status");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::Ui ui;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawFrames(imgui, controller, ui); // "Waiting for the model seat: the resolve call"; the seat window docked.
    CHECK(controller.Seat().Pending());

    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawFrames(imgui, controller, ui);
}

TEST_CASE("Withdraw in the Model seat window cancels the turn whose call waits", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_withdraw");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);

    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawSeatWindow(imgui, controller, window, 2);

    // A turn runs, so Withdraw cancels it as the Play window's Cancel does: the wait wakes on the stop token.
    PressSeatItem("Withdraw");
    DrawSeatWindow(imgui, controller, window, 1);
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    DrawSeatWindow(imgui, controller, window, 1); // No call waits any more.

    // Closed with TurnCancelled: no turn, no error, and the input comes back.
    CHECK(controller.Run().History().empty());
    CHECK(controller.Run().LastError().empty());
    CHECK(controller.TakeRestoredInput() == std::optional<std::string>("I walk to the lighthouse."));
    CHECK_FALSE(controller.Seat().Pending());
}

TEST_CASE("A call that arrives brings the Model seat tab to the front without taking the keyboard focus", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_tab");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::Ui ui;
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
    controller.NewRun(controller.Worlds().at(0), "seat", 7);
    test::PumpUntil(controller, [&] { return controller.Run().Open() && controller.CanPost(); });
    DrawFrames(imgui, controller, ui);

    // The default layout docks State, Memory and Model seat in one node. Bring another tab of it to the front.
    ImGuiWindow* pSeat = ImGui::FindWindowByName(app::MODEL_SEAT_WINDOW);
    ImGuiWindow* pState = ImGui::FindWindowByName(app::STATE_WINDOW);
    REQUIRE(pSeat != nullptr);
    REQUIRE(pState != nullptr);
    REQUIRE(pSeat->DockNode != nullptr);
    REQUIRE(pSeat->DockNode == pState->DockNode);
    REQUIRE(pSeat->DockNode->TabBar != nullptr);
    // The owner is in the Play window (docked in another node), and another tab of the seat's node is at the front.
    ImGuiWindow* pPlay = ImGui::FindWindowByName(app::PLAY_WINDOW);
    REQUIRE(pPlay != nullptr);
    ImGui::FocusWindow(pPlay);
    DrawFrames(imgui, controller, ui);
    REQUIRE(ImGui::GetCurrentContext()->NavWindow == pPlay);
    pSeat->DockNode->TabBar->SelectedTabId = pState->TabId;
    pSeat->DockNode->SelectedTabId = pState->TabId;
    DrawFrames(imgui, controller, ui);
    REQUIRE(pSeat->DockNode->TabBar->SelectedTabId == pState->TabId);
    REQUIRE(ImGui::GetCurrentContext()->NavWindow == pPlay);

    // A call waits: the seat's tab comes to the front, and the focused window does not change.
    test::SeatOnlyFor(controller.Seat(), app::SeatLabel::Resolve, app::SeatMode::Human);
    controller.Submit("I walk to the lighthouse.");
    test::PumpUntil(controller, [&] { return controller.Seat().Pending().has_value(); });
    DrawFrames(imgui, controller, ui);
    REQUIRE(pSeat->DockNode != nullptr);
    CHECK(pSeat->DockNode->TabBar->SelectedTabId == pSeat->TabId);
    CHECK(ImGui::GetCurrentContext()->NavWindow == pPlay);

    controller.Cancel();
    test::PumpUntil(controller, [&] { return controller.CanPost(); });
}

TEST_CASE("The Model seat window keeps the typed reply when the same call comes back under a new id", "[ui]")
{
    test::HeadlessImGui imgui;
    const std::filesystem::path dir = test::SettingsFolder("ui_seat_keep");
    app::AppController controller(test::PathsIn(dir), test::ScriptedFactory(Responder()));
    app::ModelSeatWindow window;
    test::FakeBackend backend(16384);
    test::PumpUntil(controller, [&] { return controller.CanPost(); });

    app::SeatRequest request;
    request.label = app::SeatLabel::Narrate;
    request.prompt = {{{inference::Role::User, "I walk."}}, 0};
    app::ModelSeat& seat = controller.Seat();

    // Types `text` into the reply box, as a keyboard would: moves the mouse down the window until it hovers the box,
    // clicks it, and sends the characters.
    auto type = [&](const char* text) {
        ImGuiWindow* pWindow = ImGui::FindWindowByName(app::MODEL_SEAT_WINDOW);
        REQUIRE(pWindow != nullptr);
        const ImGuiID replyId = pWindow->GetID("##reply");
        ImGuiIO& io = ImGui::GetIO();
        bool found = false;
        for (float y = 5.0f; y < 800.0f && !found; y += 5.0f) {
            io.AddMousePosEvent(300.0f, y);
            DrawSeatWindow(imgui, controller, window, 1);
            found = ImGui::GetCurrentContext()->HoveredId == replyId;
        }
        REQUIRE(found);
        io.AddMouseButtonEvent(0, true);
        DrawSeatWindow(imgui, controller, window, 1);
        io.AddMouseButtonEvent(0, false);
        DrawSeatWindow(imgui, controller, window, 1);
        REQUIRE(ImGui::GetCurrentContext()->ActiveId == replyId);
        io.AddInputCharactersUTF8(text);
        DrawSeatWindow(imgui, controller, window, 2);
    };

    // The first call: the owner types a reply, then the call is preempted (withdrawn at the seat).
    {
        test::AsyncCall<std::optional<app::SeatReply>> call(
            [&](std::stop_token stop) { return seat.Await(request, backend, stop); });
        const app::SeatRequest first = test::WaitForRequest(seat);
        DrawSeatWindow(imgui, controller, window, 2);
        type("typed");
        CHECK(seat.Withdraw(first.id));
        CHECK_FALSE(call.Get());
    }

    // The same call comes back under a new id: the typed text stays.
    {
        test::AsyncCall<std::optional<app::SeatReply>> call(
            [&](std::stop_token stop) { return seat.Await(request, backend, stop); });
        const app::SeatRequest second = test::WaitForRequest(seat);
        CHECK(second.id > 0);
        DrawSeatWindow(imgui, controller, window, 2);
        PressSeatItem("Submit");
        DrawSeatWindow(imgui, controller, window, 1);
        const std::optional<app::SeatReply> reply = call.Get();
        REQUIRE(reply);
        CHECK(reply->text == "typed");
    }

    // Another call (other prompt): the boxes start over.
    {
        app::SeatRequest other = request;
        other.prompt.messages[0].content = "I run.";
        test::AsyncCall<std::optional<app::SeatReply>> call(
            [&](std::stop_token stop) { return seat.Await(other, backend, stop); });
        test::WaitForRequest(seat);
        DrawSeatWindow(imgui, controller, window, 2);
        PressSeatItem("Submit");
        DrawSeatWindow(imgui, controller, window, 1);
        const std::optional<app::SeatReply> reply = call.Get();
        REQUIRE(reply);
        CHECK(reply->text.empty());
    }
}
