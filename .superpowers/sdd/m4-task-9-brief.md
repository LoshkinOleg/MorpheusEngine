### Task 9: The UI shell, the Runs window and the Play window

This task creates `morpheus_app_ui`:
- `Ui` draws a status bar and a dock space over the main viewport, building the default layout when no saved layout exists, then draws each window;
- the Runs window starts and continues runs;
- the Play window shows the transcript with inline rolls and the streaming narration, and holds the input box and the Undo, Regenerate and Cancel buttons.

Headless tests draw the UI through a whole run.

**Files:**
- Create: `src/app/ui/Windows.h`, `src/app/ui/Ui.h`, `src/app/ui/Ui.cpp`, `src/app/ui/RunsWindow.cpp`, `src/app/ui/PlayWindow.cpp`, `tests/app/UiTests.cpp`
- Modify: `src/app/CMakeLists.txt` (add `morpheus_app_ui`), `tests/CMakeLists.txt` (link it)

**Interfaces:**
- Consumes:
  - `AppController` (Task 8), `RunModel` (Task 5) and the `Catalog` functions (Task 3);
  - `context::RenderMechanicsLine` and `core::Trim`;
  - `ImGui::InputText` and `InputTextMultiline` with `std::string*` (`imgui_stdlib.h`);
  - from `imgui_internal.h`: `DockBuilder*` and `BeginViewportSideBar`.
- Produces:
  - the window name constants `RUNS_WINDOW`, `PLAY_WINDOW`, `STATE_WINDOW`, `CONTEXT_WINDOW`, `TURN_LOG_WINDOW`, `MEMORY_WINDOW` and `SETTINGS_WINDOW`;
  - the color constants and `void TextWrappedColored(const ImVec4&, std::string_view)`;
  - `class RunsWindow` and `class PlayWindow`, each with `void Draw(AppController&)`;
  - `class Ui { void Draw(AppController&); }`.

- [ ] **Step 1: Write the failing tests**

Create `tests/app/UiTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "app/AppTestSupport.h"
#include "app/HeadlessImGui.h"
#include "app/model/AppController.h"
#include "app/ui/Ui.h"
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
```

- [ ] **Step 2: Run the tests to verify they fail**

In `src/app/CMakeLists.txt`, after `morpheus_app_model`, add:

```cmake
# The ImGui windows over AppController. They need only ImGui's core, so the tests draw them headless.
add_library(morpheus_app_ui STATIC
    ui/Ui.cpp
    ui/RunsWindow.cpp
    ui/PlayWindow.cpp
)
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_ui PUBLIC morpheus_app_model imgui_core)
morpheus_target_defaults(morpheus_app_ui)
```

In `tests/CMakeLists.txt`, add `morpheus_app_ui` to the `target_link_libraries(morpheus_tests PRIVATE ...)` line.

Run: `cmake --build --preset tests`
Expected: a compile error: `app/ui/Ui.h` is missing.

- [ ] **Step 3: Write the window declarations**

Create `src/app/ui/Windows.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "app/model/AppController.h"
#include "app/model/Catalog.h"

namespace morpheus::app {

// Window names. ImGui identifies windows, and their places in imgui.ini, by them.
constexpr const char* RUNS_WINDOW = "Runs";
constexpr const char* PLAY_WINDOW = "Play";
constexpr const char* STATE_WINDOW = "State";
constexpr const char* CONTEXT_WINDOW = "Context";
constexpr const char* TURN_LOG_WINDOW = "Turn log";
constexpr const char* MEMORY_WINDOW = "Memory";
constexpr const char* SETTINGS_WINDOW = "Settings";

// Text colors.
constexpr ImVec4 ERROR_COLOR = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 WARNING_COLOR = ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
constexpr ImVec4 INPUT_COLOR = ImVec4(0.55f, 0.8f, 1.0f, 1.0f);     // The player's input in the transcript.
constexpr ImVec4 MECHANICS_COLOR = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);  // Rolls and changes in the transcript.
constexpr ImVec4 CHANGED_COLOR = ImVec4(0.45f, 1.0f, 0.45f, 1.0f);  // A state node the latest turn changed.
constexpr ImVec4 CONTAINS_COLOR = ImVec4(0.75f, 0.9f, 0.6f, 1.0f);  // A state node holding a change.

// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);

// Worlds, their saved runs, and starting or continuing one.
class RunsWindow {
private:
    std::size_t _selectedWorld = 0; // Index into AppController::Worlds().
    bool _suggestName = true;       // Suggest a fresh run name on the next draw.
    std::string _runName = {};
    std::string _seedText = {};     // Blank: a random seed.
    std::vector<RunEntry> _runs = {};
    std::string _runsKey = {};      // What _runs was listed for; listing again when it changes.

public:
    void Draw(AppController& controller);
};

// The story: the transcript with inline rolls, the streaming narration, the input box, and undo, regenerate and
// cancel.
class PlayWindow {
private:
    std::string _input = {};
    bool _couldPost = false; // Last frame's CanPost: the input takes focus when the engine is ready again.

public:
    void Draw(AppController& controller);
};

} // namespace morpheus::app
```

Create `src/app/ui/Ui.h`:

```cpp
#pragma once

#include "app/model/AppController.h"
#include "app/ui/Windows.h"

namespace morpheus::app {

// Every window, docked over the main viewport, with a status bar along the bottom. Draw once per frame between
// ImGui::NewFrame and ImGui::Render, after AppController::Frame.
class Ui {
private:
    RunsWindow _runs = {};
    PlayWindow _play = {};

public:
    void Draw(AppController& controller);
};

} // namespace morpheus::app
```

- [ ] **Step 4: Write the shell**

Create `src/app/ui/Ui.cpp`:

```cpp
#include "app/ui/Ui.h"

#include <format>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (the default layout) and BeginViewportSideBar (the status bar).

namespace morpheus::app {

void TextWrappedColored(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
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
    // default one: runs and settings on the left, the story in the middle, state and memory on the right, and the
    // turn log and context below the story.
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
        ImGui::DockBuilderDockWindow(TURN_LOG_WINDOW, bottom);
        ImGui::DockBuilderDockWindow(CONTEXT_WINDOW, bottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, pViewport);

    _runs.Draw(controller);
    _play.Draw(controller);
}

} // namespace morpheus::app
```

- [ ] **Step 5: Write the Runs window**

Create `src/app/ui/RunsWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/Text.h"

namespace morpheus::app {

void RunsWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(RUNS_WINDOW)) {
        // The open run.
        if (const std::optional<turn::LoadRun>& open = controller.OpenRun()) {
            ImGui::Text("Open: %s / %s", controller.Run().Meta().worldId.c_str(),
                        core::PathText(open->runFile.stem()).c_str());
        } else {
            ImGui::TextDisabled("No run open");
        }

        // The worlds. Refresh lists them again after world files were added or edited.
        ImGui::SeparatorText("Worlds");
        if (ImGui::SmallButton("Refresh")) {
            controller.RefreshWorlds();
            _selectedWorld = 0;
            _suggestName = true;
            _runsKey.clear();
        }
        const std::vector<WorldEntry>& worlds = controller.Worlds();
        if (worlds.empty()) {
            ImGui::TextDisabled("No worlds in %s", core::PathText(controller.Paths().projectsDir).c_str());
        }
        for (std::size_t i = 0; i < worlds.size(); ++i) {
            const WorldEntry& world = worlds[i];
            ImGui::PushID(static_cast<int>(i));
            if (!world.error.empty()) {
                TextWrappedColored(ERROR_COLOR, std::format("{}: {}", world.id, world.error)); // Not playable.
            } else if (ImGui::Selectable(std::format("{} ({})", world.title, world.id).c_str(), _selectedWorld == i)) {
                if (_selectedWorld != i) {
                    _selectedWorld = i;
                    _suggestName = true;
                }
            }
            ImGui::PopID();
        }
        if (_selectedWorld >= worlds.size() || !worlds[_selectedWorld].error.empty()) {
            ImGui::TextDisabled("Select a world to start or continue a run.");
        } else {
            const WorldEntry& world = worlds[_selectedWorld];

            // A new run: a name (suggested from the clock) and an optional seed.
            ImGui::SeparatorText("New run");
            if (_suggestName) {
                _runName = DefaultRunName(std::chrono::system_clock::now());
                _suggestName = false;
            }
            ImGui::InputText("Name", &_runName);
            ImGui::InputTextWithHint("Seed", "random", &_seedText);
            const std::string nameProblem = RunNameProblem(controller.Paths().savesDir, world.id, _runName);
            const SeedInput seed = ParseSeed(_seedText);
            const std::string problem = nameProblem.empty() ? seed.problem : nameProblem;
            if (!problem.empty()) {
                TextWrappedColored(ERROR_COLOR, problem);
            }
            ImGui::BeginDisabled(!controller.CanPost() || !problem.empty());
            if (ImGui::Button("Start")) {
                controller.NewRun(world, _runName, seed.seed);
                _suggestName = true;
                _seedText.clear();
            }
            ImGui::EndDisabled();

            // The world's saved runs. They are listed again when the world, the open run or its turn count changes:
            // a turn writes its run's file, which moves it to the top.
            const std::string runsKey =
                std::format("{}|{}|{}", world.id,
                            controller.OpenRun() ? core::PathText(controller.OpenRun()->runFile) : std::string(),
                            controller.Run().History().size());
            if (runsKey != _runsKey) {
                _runs = ListRuns(controller.Paths().savesDir, world.id);
                _runsKey = runsKey;
            }
            ImGui::SeparatorText("Saved runs");
            if (_runs.empty()) {
                ImGui::TextDisabled("No runs yet.");
            }
            for (const RunEntry& run : _runs) {
                ImGui::PushID(run.name.c_str());
                ImGui::BeginDisabled(!controller.CanPost());
                if (ImGui::SmallButton("Continue")) {
                    controller.ContinueRun(world, run);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(run.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", LocalTimeText(run.lastWrite).c_str());
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 6: Write the Play window**

Create `src/app/ui/PlayWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "context/Transcript.h"
#include "core/Text.h"

namespace morpheus::app {

namespace {

constexpr float INPUT_LINES = 4.0f; // Height of the input box, in lines of text.

} // namespace

void PlayWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(PLAY_WINDOW)) {
        const RunModel& run = controller.Run();
        if (!run.Open()) {
            ImGui::TextDisabled("No run open: start or continue one in the Runs window.");
            if (!run.LastError().empty()) {
                TextWrappedColored(ERROR_COLOR, run.LastError());
            }
        } else {
            if (run.WorldChanged()) {
                TextWrappedColored(WARNING_COLOR, "The world's files changed since this run started.");
            }

            // One turn as the transcript shows it: the input, the mechanics line (rolls and changes), the narration.
            auto drawTurn = [](const std::string& input, const std::string& mechanics, const std::string& narration) {
                TextWrappedColored(INPUT_COLOR, "> " + input);
                if (!mechanics.empty()) {
                    TextWrappedColored(MECHANICS_COLOR, mechanics);
                }
                ImGui::TextWrapped("%s", narration.c_str());
                ImGui::Spacing();
            };

            // The transcript fills the window above the input box and the buttons.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float inputHeight = ImGui::GetTextLineHeight() * INPUT_LINES + style.FramePadding.y * 2.0f;
            const float footerHeight = inputHeight + style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
            if (ImGui::BeginChild("transcript", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders)) {
                // Follow new text while the view is at the bottom; scrolling up to read stops following.
                const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
                for (const core::TurnRecord& turn : run.TranscriptTurns()) {
                    drawTurn(turn.input, context::RenderMechanicsLine(turn), turn.narration);
                }
                if (const std::optional<RunModel::PendingTurn>& pending = run.Pending()) {
                    const std::string mechanics = pending->resolved ? context::RenderMechanicsLine(*pending->resolved)
                                                                    : std::string("(resolving...)");
                    drawTurn(pending->input, mechanics,
                             pending->narration.empty() ? std::string("...") : pending->narration);
                }
                if (!run.LastError().empty()) {
                    TextWrappedColored(ERROR_COLOR, run.LastError());
                }
                if (!run.MaintenanceError().empty()) {
                    TextWrappedColored(WARNING_COLOR, "Memory upkeep failed: " + run.MaintenanceError());
                }
                if (atBottom) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();

            // The input box: Enter plays the turn, Ctrl+Enter starts a new line. A cancelled or failed turn's input
            // comes back to it when it is empty.
            if (std::optional<std::string> restored = controller.TakeRestoredInput(); restored && _input.empty()) {
                _input = std::move(*restored);
            }
            const bool canPost = controller.CanPost();
            if (canPost && !_couldPost) {
                ImGui::SetKeyboardFocusHere(); // Ready again: type the next action straight away.
            }
            _couldPost = canPost;
            ImGui::BeginDisabled(!canPost);
            const bool entered =
                ImGui::InputTextMultiline("##input", &_input, ImVec2(-FLT_MIN, inputHeight),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
            const bool played = ImGui::Button("Play");
            ImGui::SameLine();
            ImGui::BeginDisabled(run.History().empty());
            const bool undo = ImGui::Button("Undo");
            ImGui::SameLine();
            const bool regenerate = ImGui::Button("Regenerate");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool turnRunning =
                run.CurrentBusy() == RunModel::Busy::Turn || run.CurrentBusy() == RunModel::Busy::Regenerate;
            ImGui::BeginDisabled(!turnRunning);
            const bool cancel = ImGui::Button("Cancel");
            ImGui::EndDisabled();

            // Act after drawing, so one frame posts at most one command.
            if (cancel) {
                controller.Cancel();
            } else if (undo) {
                controller.Undo();
            } else if (regenerate) {
                controller.Regenerate();
            } else if ((entered || played) && canPost && !core::Trim(_input).empty()) {
                controller.Submit(std::exchange(_input, std::string()));
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[ui]"`
Expected: both tests pass. An `ImGui assertion failed` error means the UI broke an ImGui rule (an unbalanced push or pop, or Begin/End); fix the UI code, not the assertion. Then `ctest --preset tests`: everything passes.

- [ ] **Step 8: Commit**

```bash
git add src/app/CMakeLists.txt tests/CMakeLists.txt src/app/ui/Windows.h src/app/ui/Ui.h src/app/ui/Ui.cpp src/app/ui/RunsWindow.cpp src/app/ui/PlayWindow.cpp tests/app/UiTests.cpp
git commit -m "Add the UI shell with the Runs and Play windows" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

