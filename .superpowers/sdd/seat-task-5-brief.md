### Task 5: The Model seat window

The window shows the switch and the modes always, and a waiting call with its limits, prompt, grammar and draft, a reply and a thinking box, and Check, Submit and Withdraw. Withdraw releases any pending request: while a turn or regenerate runs it cancels the command (the Play window's Cancel), otherwise (between-turn maintenance) it calls `ModelSeat::Withdraw`. There is no separate Cancel turn button. The Context window's prompt drawing becomes `DrawPrompt`, which both windows call. The status bar says when the engine waits for the seat.

**Files:**
- Create: `src/app/ui/ModelSeatWindow.cpp`
- Modify: `src/app/ui/Windows.h`, `src/app/ui/Ui.h`, `src/app/ui/Ui.cpp`, `src/app/ui/ContextWindow.cpp`, `src/app/CMakeLists.txt`
- Test: `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes: `AppController::Seat()`, `AppController::Cancel()` (Task 4, existing); `ModelSeat` (`Submit(id, ...)`, `Check(id, ...)`, `Withdraw(id)`), `SEAT_LABELS`, `SeatLabelName`, `SeatRequest`, `SeatCheck` (Task 2); `test::SeatOnlyFor` (Task 2); `test::ScriptedBackend::GrammarChecks` (Task 1).
- Produces:
  - `constexpr const char* MODEL_SEAT_WINDOW = "Model seat";`
  - `void DrawPrompt(const inference::Prompt& prompt, bool& showRaw);` (in `Ui.cpp`, declared in `Windows.h`).
  - `class ModelSeatWindow { public: void Draw(AppController& controller); };` with items labelled exactly `Answer as the model` (checkbox), `Check`, `Submit`, `Withdraw`, drawn directly in the window (the tests find them by those IDs).

- [ ] **Step 1: Write the failing tests**

In `tests/app/UiTests.cpp`, replace

```cpp
#include <algorithm>
#include <chrono>
```

with

```cpp
#include <algorithm>
#include <atomic>
#include <chrono>
```

replace

```cpp
#include <string>
#include <vector>

#include "SceneWorld.h"
#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/model/AppController.h"
#include "app/model/Inspect.h"
```

with

```cpp
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
```

replace

```cpp
#include "app/ui/Ui.h"
```

with

```cpp
#include "app/ui/Ui.h"
#include "app/ui/Windows.h"
#include "fakes/ScriptedBackend.h"
```

inside the anonymous namespace, after `DrawFrames`, add:

```cpp
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
```

and append:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/app/UiTests.cpp`: `'ModelSeatWindow': is not a member of 'morpheus::app'` and `'MODEL_SEAT_WINDOW': is not a member of 'morpheus::app'`.

- [ ] **Step 3: Declare the window and the shared prompt drawing**

In `src/app/ui/Windows.h`, replace

```cpp
#include <cstddef>
#include <optional>
```

with

```cpp
#include <cstddef>
#include <cstdint>
#include <optional>
```

replace

```cpp
#include "app/model/Catalog.h"
#include "app/model/SettingsEdit.h"
```

with

```cpp
#include "app/model/Catalog.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "inference/Llm.h"
```

replace

```cpp
constexpr const char* SETTINGS_WINDOW = "Settings";
```

with

```cpp
constexpr const char* SETTINGS_WINDOW = "Settings";
constexpr const char* MODEL_SEAT_WINDOW = "Model seat";
```

replace

```cpp
// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);
```

with

```cpp
// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);

// A prompt as the Context window shows it, in the rest of the window: Sections / Raw radio buttons, then one
// collapsing section per message (titled by its role and layer) or the whole text, read-only. The choice is kept in
// `showRaw`. Used by the Context and Model seat windows.
void DrawPrompt(const inference::Prompt& prompt, bool& showRaw);
```

and append, after the `SettingsWindow` class (before the namespace's closing brace):

```cpp

// Debug: the owner answers the engine's model calls by hand (the model seat). Always: the "Answer as the model"
// switch and each label's Human / Draft / Model mode. While a call waits: its label and limits, the model's thinking
// (Draft mode) and the grammar, the reply and thinking boxes, Check, Submit and Withdraw, and the prompt as the
// Context window draws it. Withdraw releases the call: a running turn's or regenerate's by cancelling the command,
// a between-turn fold's or condense's at the seat. The texts typed stay until Submit and are reset when a new call
// arrives.
class ModelSeatWindow {
private:
    std::uint64_t _requestId = 0;         // The request _reply and _thinking belong to (SeatRequest::id); 0: none yet.
    std::string _reply = {};
    std::string _thinking = {};
    std::optional<SeatCheck> _check = {}; // The last Check of the reply as it is now; cleared when it is edited.
    bool _showRaw = false;                // The prompt as raw text instead of sections.

public:
    void Draw(AppController& controller);
};
```

- [ ] **Step 4: DrawPrompt, the status bar, the dock place and the draw call**

Replace the whole of `src/app/ui/Ui.cpp` with:

```cpp
#include "app/ui/Ui.h"

#include <cfloat>
#include <format>
#include <optional>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (the default layout) and BeginViewportSideBar (the status bar).
#include <imgui_stdlib.h>

#include "app/model/Inspect.h"
#include "app/model/ModelSeat.h"

namespace morpheus::app {

void TextWrappedColored(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawPrompt(const inference::Prompt& prompt, bool& showRaw)
{
    // The view: one section per message, or the whole text to copy.
    if (ImGui::RadioButton("Sections", !showRaw)) {
        showRaw = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Raw", showRaw)) {
        showRaw = true;
    }
    if (showRaw) {
        std::string text = RawPrompt(prompt);
        ImGui::InputTextMultiline("##raw", &text, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    } else {
        if (ImGui::BeginChild("sections")) {
            for (const PromptSection& section : PromptSections(prompt)) {
                if (ImGui::CollapsingHeader(section.title.c_str())) {
                    ImGui::TextWrapped("%s", section.text.c_str());
                }
            }
        }
        ImGui::EndChild();
    }
}

void Ui::Draw(AppController& controller)
{
    ImGuiViewport* pViewport = ImGui::GetMainViewport();

    // The status bar along the bottom: what the engine and the run are doing. It is drawn before the dock space,
    // which then fills the rest of the viewport.
    std::string status;
    switch (controller.Engine().CurrentStatus()) {
    case EngineHost::Status::Stopped:
        status = "Engine stopped";
        break;
    case EngineHost::Status::Loading:
        status = "Loading models...";
        break;
    case EngineHost::Status::Failed:
        status = "Engine failed: " + controller.Engine().Failure() + " (fix it in Settings and apply)";
        break;
    case EngineHost::Status::Ready:
        // A call waiting at the model seat holds the engine until it is answered or cancelled.
        if (const std::optional<SeatRequest> request = controller.Seat().Pending()) {
            status = std::format("Waiting for the model seat: the {} call", SeatLabelName(request->label));
            break;
        }
        switch (controller.Run().CurrentBusy()) {
        case RunModel::Busy::None:
            status = controller.Run().Open() ? std::format("Ready - {}, {} turns", controller.Run().Meta().worldId,
                                                           controller.Run().History().size())
                                             : std::string("Ready - no run open");
            break;
        case RunModel::Busy::Opening:
            status = "Opening the run...";
            break;
        case RunModel::Busy::Turn:
            status = "Playing the turn...";
            break;
        case RunModel::Busy::Regenerate:
            status = "Playing the turn again...";
            break;
        case RunModel::Busy::Undo:
            status = "Undoing the turn...";
            break;
        }
        break;
    }
    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", pViewport, ImGuiDir_Down, ImGui::GetFrameHeight(), barFlags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted(status.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    // The dock space. Without a saved layout (first launch, or imgui.ini deleted), the windows are docked into the
    // default one: runs and settings on the left, the story in the middle, state, memory and the model seat on the
    // right, and the turn log and context below the story.
    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, pViewport->WorkSize);
        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGui::DockBuilderDockWindow(RUNS_WINDOW, left);
        ImGui::DockBuilderDockWindow(SETTINGS_WINDOW, left);
        ImGui::DockBuilderDockWindow(PLAY_WINDOW, center);
        ImGui::DockBuilderDockWindow(STATE_WINDOW, right);
        ImGui::DockBuilderDockWindow(MEMORY_WINDOW, right);
        ImGui::DockBuilderDockWindow(MODEL_SEAT_WINDOW, right);
        ImGui::DockBuilderDockWindow(TURN_LOG_WINDOW, bottom);
        ImGui::DockBuilderDockWindow(CONTEXT_WINDOW, bottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, pViewport);

    _runs.Draw(controller);
    _play.Draw(controller);
    _state.Draw(controller);
    _turnLog.Draw(controller);
    _context.Draw(controller);
    _memory.Draw(controller);
    _settings.Draw(controller);
    _modelSeat.Draw(controller);
}

} // namespace morpheus::app
```

In `src/app/ui/Ui.h`, replace

```cpp
    SettingsWindow _settings = {};
```

with

```cpp
    SettingsWindow _settings = {};
    ModelSeatWindow _modelSeat = {};
```

- [ ] **Step 5: The Context window calls DrawPrompt**

Replace the whole of `src/app/ui/ContextWindow.cpp` with:

```cpp
#include "app/ui/Windows.h"

#include <optional>
#include <utility>

#include <imgui.h>

#include "app/model/Inspect.h"

namespace morpheus::app {

namespace {

constexpr const char* CALLS[] = {"lookup", "resolve", "narrate"}; // Model calls, in turn order.

} // namespace

void ContextWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(CONTEXT_WINDOW)) {
        const RunModel& run = controller.Run();
        const std::optional<context::Assembly>& assembly = run.LastAssembly();
        const core::TurnRecord* pLatest = run.LatestTurn();
        if (!assembly || !pLatest) {
            ImGui::TextDisabled("No prompt yet: it shows once a turn is played in this session.");
        } else {
            ImGui::Text("The resolve prompt of turn %d", pLatest->number);

            // Tokens per layer. Layer counts are raw text; the total is the whole templated prompt.
            const context::LayerTokens& tokens = assembly->tokens;
            const std::pair<const char*, int> layers[] = {
                {"L0 engine rules", tokens.engineRules}, {"L1 world", tokens.world},
                {"L2 summary", tokens.summary},          {"L3 transcript", tokens.transcript},
                {"Tail (T1-T4)", tokens.tail},           {"Total (templated)", tokens.total}};
            if (ImGui::BeginTable("layers", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Layer");
                ImGui::TableSetupColumn("Tokens");
                ImGui::TableHeadersRow();
                for (const auto& [name, count] : layers) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", count);
                }
                ImGui::EndTable();
            }
            ImGui::Text("Transcript: %d of %d budget tokens", assembly->transcriptCost, assembly->transcriptBudget);
            if (!assembly->Fits()) {
                TextWrappedColored(WARNING_COLOR, "This prompt was over budget.");
            }

            // KV-cache reuse per call of the latest turn.
            if (ImGui::BeginTable("cache", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                for (const char* header : {"Call", "Prompt", "Reused", "Evaluated"}) {
                    ImGui::TableSetupColumn(header);
                }
                ImGui::TableHeadersRow();
                for (const char* call : CALLS) {
                    const std::optional<CallStats> stats = CallStatsOf(pLatest->timings, call);
                    if (!stats) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(call);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->promptTokens);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->reused);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", stats->evaluated);
                }
                ImGui::EndTable();
            }

            // The prompt itself, sections or raw, in the rest of the window.
            ImGui::SeparatorText("Prompt");
            DrawPrompt(assembly->prompt, showRaw);
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 6: Write the window**

Create `src/app/ui/ModelSeatWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/model/ModelSeat.h"

namespace morpheus::app {

namespace {

constexpr float REPLY_LINES = 8.0f;    // Height of the reply box, in lines of text.
constexpr float THINKING_LINES = 4.0f; // Height of the thinking box, in lines of text.

} // namespace

void ModelSeatWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(MODEL_SEAT_WINDOW)) {
        ModelSeat& seat = controller.Seat();

        // The switch: while it is off, the model answers every call as usual.
        bool enabled = seat.Enabled();
        if (ImGui::Checkbox("Answer as the model", &enabled)) {
            seat.SetEnabled(enabled);
        }

        // Who answers each kind of call: the owner (Human), the owner from the model's draft (Draft), or the model.
        if (ImGui::BeginTable("modes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* header : {"Call", "Human", "Draft", "Model"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (const SeatLabel label : SEAT_LABELS) {
                ImGui::PushID(static_cast<int>(label));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(SeatLabelName(label).c_str());
                for (const SeatMode mode : {SeatMode::Human, SeatMode::Draft, SeatMode::Model}) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(mode));
                    if (ImGui::RadioButton("##mode", seat.ModeOf(label) == mode)) {
                        seat.SetMode(label, mode);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        const std::optional<SeatRequest> request = seat.Pending();
        if (!request) {
            ImGui::TextDisabled("%s", seat.Enabled() ? "No call waits for a reply." : "Off: the model answers every call.");
        } else {
            // A new call: the reply starts as the model's draft (Draft mode) or empty (Human mode).
            if (request->id != _requestId) {
                _requestId = request->id;
                _reply = request->draft ? request->draft->text : std::string();
                _thinking = request->draft ? request->draft->thinking : std::string();
                _check.reset();
            }
            const inference::GenerationSpec& spec = request->spec;

            // Which call waits, and the limits the model would be held to.
            ImGui::SeparatorText(std::format("The {} call waits for a reply", SeatLabelName(request->label)).c_str());
            if (spec.thinkingMaxTokens > 0) {
                ImGui::Text("At most %d answer tokens, after at most %d thinking tokens", spec.maxTokens,
                            spec.thinkingMaxTokens);
            } else {
                ImGui::Text("At most %d answer tokens", spec.maxTokens);
            }
            if (!seat.Enabled()) {
                TextWrappedColored(WARNING_COLOR, "The seat is off: this call still waits for a reply; later calls go "
                                                  "to the model.");
            }

            // The model's thinking (Draft mode, read-only) and the grammar, folded away until opened.
            if (request->draft && !request->draft->thinking.empty() && ImGui::CollapsingHeader("The model's thinking")) {
                ImGui::TextWrapped("%s", request->draft->thinking.c_str());
            }
            if (!spec.grammar.empty() && ImGui::CollapsingHeader("Grammar")) {
                ImGui::TextUnformatted(spec.grammar.c_str());
            }

            // The reply, and the thinking when the call may think. Edits stay in this window until Submit.
            const float padding = ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::TextUnformatted("Reply");
            if (ImGui::InputTextMultiline("##reply", &_reply,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * REPLY_LINES + padding))) {
                _check.reset(); // The last check was of another text.
            }
            if (spec.thinkingMaxTokens > 0) {
                ImGui::TextUnformatted("Thinking");
                ImGui::InputTextMultiline("##thinking", &_thinking,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * THINKING_LINES + padding));
            }

            // Check (only with a grammar), Submit (whether or not the reply fits), and Withdraw (always: it releases
            // the call without a reply).
            ImGui::BeginDisabled(spec.grammar.empty());
            const bool check = ImGui::Button("Check");
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool submit = ImGui::Button("Submit");
            ImGui::SameLine();
            const bool withdraw = ImGui::Button("Withdraw");
            if (_check) {
                ImGui::Text("%d of %d answer tokens", _check->tokens, spec.maxTokens);
                if (_check->problem.empty()) {
                    ImGui::TextUnformatted("Fits the grammar");
                } else {
                    TextWrappedColored(WARNING_COLOR, _check->problem);
                }
            }

            // The prompt, as the Context window draws it, in the rest of the window.
            ImGui::SeparatorText("Prompt");
            DrawPrompt(request->prompt, _showRaw);

            // Act after drawing, one action per frame, on the request drawn (by its id). A request a cancel ended
            // since Pending() is stale: Submit and Withdraw then return false and Check nullopt, and nothing is left
            // to do.
            if (withdraw) {
                const RunModel::Busy busy = controller.Run().CurrentBusy();
                if (busy == RunModel::Busy::Turn || busy == RunModel::Busy::Regenerate) {
                    // A running turn's call (a fold of its fit loop, or of the maintenance before it, included):
                    // cancel the command, as the Play window's Cancel does, so the turn closes with TurnCancelled.
                    controller.Cancel();
                } else {
                    // Between-turn maintenance: release the fold or condense at the seat. The maintenance stops as a
                    // preempted one does, and the next turn folds what it needs.
                    seat.Withdraw(request->id);
                }
            } else if (submit) {
                seat.Submit(request->id, std::exchange(_reply, std::string()), std::exchange(_thinking, std::string()));
                _check.reset();
            } else if (check) {
                _check = seat.Check(request->id, _reply);
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 7: Add the source to the build**

In `src/app/CMakeLists.txt`, replace

```cmake
    ui/SettingsWindow.cpp
)
```

with

```cmake
    ui/SettingsWindow.cpp
    ui/ModelSeatWindow.cpp
)
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[ui]"`
Expected: all `[ui]` tests pass, the five new ones included ("The inspectors draw ..." still draws the Context window's raw view through `DrawPrompt`).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 311`.

- [ ] **Step 9: Build the app**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus`
Expected: builds with no warnings (`main.cpp` is unchanged: `AppController` wraps the llama.cpp factory).

- [ ] **Step 10: Commit**

```bash
git add src/app/ui/Windows.h src/app/ui/Ui.h src/app/ui/Ui.cpp src/app/ui/ContextWindow.cpp src/app/ui/ModelSeatWindow.cpp src/app/CMakeLists.txt tests/app/UiTests.cpp
git commit -m "Add the Model seat window" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

