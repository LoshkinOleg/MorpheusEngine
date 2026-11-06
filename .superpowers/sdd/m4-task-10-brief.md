### Task 10: The inspectors and the Settings window

This task adds the remaining spec windows:
- **State:** the state tree, with the latest turn's changes marked.
- **Turn log:** each turn's input, intent, rolls, applied and rejected changes, notes, lookups and per-call numbers, plus the raw resolve JSON.
- **Context:** the latest resolve prompt by layer, with tokens per layer, cache reuse per call, and a formatted or raw view.
- **Memory:** summaries, notes, and the latest turn's retrieval hits with scores.
- **Settings:** the editable settings, with Apply and Revert.

**Files:**
- Create: `src/app/ui/StateWindow.cpp`, `src/app/ui/TurnLogWindow.cpp`, `src/app/ui/ContextWindow.cpp`, `src/app/ui/MemoryWindow.cpp`, `src/app/ui/SettingsWindow.cpp`
- Modify: `src/app/ui/Windows.h`, `src/app/ui/Ui.h`, `src/app/ui/Ui.cpp`, `src/app/CMakeLists.txt`, `tests/app/UiTests.cpp`

**Interfaces:**
- Consumes:
  - the `Inspect` functions (Task 6) and `SettingsEdit` (Task 4);
  - `AppController::CanApplySettings` and `AppController::ApplySettings` (Task 8);
  - `context::RenderChangePart` and `core::CutUtf8`.
- Produces: `class StateWindow`, `class TurnLogWindow`, `class ContextWindow` (with the public data member `bool showRaw`), `class MemoryWindow` and `class SettingsWindow`, each with `void Draw(AppController&)`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/app/UiTests.cpp`. Include `"app/model/SettingsEdit.h"` and `"core/MemoryMode.h"`, and put the new helper in the anonymous namespace:

```cpp
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
```

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Add `ui/StateWindow.cpp`, `ui/TurnLogWindow.cpp`, `ui/ContextWindow.cpp`, `ui/MemoryWindow.cpp` and `ui/SettingsWindow.cpp` to `morpheus_app_ui` in `src/app/CMakeLists.txt`.

Run: `cmake --build --preset tests`
Expected: a compile error: `ContextWindow` is not a member of `morpheus::app`, and the new source files are missing.

- [ ] **Step 3: Declare the windows**

Add to `src/app/ui/Windows.h`, after `PlayWindow`. Include `<optional>` and `"app/model/SettingsEdit.h"`.

```cpp
// The state tree, with the latest turn's changes marked.
class StateWindow {
public:
    void Draw(AppController& controller);
};

// Every turn, newest first: input, intent, rolls, applied and rejected changes, notes, lookups, per-call numbers and
// the raw resolve output.
class TurnLogWindow {
public:
    void Draw(AppController& controller);
};

// The latest resolve prompt by layer: tokens per layer, cache reuse per call, and the text as sections or raw.
class ContextWindow {
public:
    bool showRaw = false; // The prompt as one raw text instead of sections (the window's toggle).

    void Draw(AppController& controller);
};

// The story summaries, the notes, and the latest turn's retrieval hits with their scores.
class MemoryWindow {
public:
    void Draw(AppController& controller);
};

// The editable settings. Apply saves settings.json and restarts the engine; Revert reads the file again.
class SettingsWindow {
private:
    std::optional<SettingsEdit> _edit = {}; // Read from the file on the first draw and after Revert.
    std::string _problem = {};              // Why the file could not be read, or why Apply refused the edit.

public:
    void Draw(AppController& controller);
};
```

In `src/app/ui/Ui.h`, add the members after `_play`:

```cpp
    StateWindow _state = {};
    TurnLogWindow _turnLog = {};
    ContextWindow _context = {};
    MemoryWindow _memory = {};
    SettingsWindow _settings = {};
```

In `src/app/ui/Ui.cpp`, after `_play.Draw(controller);`:

```cpp
    _state.Draw(controller);
    _turnLog.Draw(controller);
    _context.Draw(controller);
    _memory.Draw(controller);
    _settings.Draw(controller);
```

- [ ] **Step 4: Write the State window**

Create `src/app/ui/StateWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "app/model/Inspect.h"

namespace morpheus::app {

void StateWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(STATE_WINDOW)) {
        const RunModel& run = controller.Run();
        if (!run.Open()) {
            ImGui::TextDisabled("No run open.");
        } else {
            // Marks come from the latest committed turn's applied changes.
            const core::TurnRecord* pLatest = run.LatestTurn();
            const std::vector<core::AppliedChange> noChanges;
            const std::vector<core::AppliedChange>& applied = pLatest ? pLatest->applied : noChanges;
            if (pLatest) {
                ImGui::TextDisabled("After turn %d. Highlighted: what it changed.", pLatest->number);
            } else {
                ImGui::TextDisabled("The world's initial state.");
            }

            // Pushes the text color for a mark. Returns whether it pushed one, which the caller then pops.
            auto pushMark = [](ChangeMark mark) {
                if (mark == ChangeMark::None) {
                    return false;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, mark == ChangeMark::Changed ? CHANGED_COLOR : CONTAINS_COLOR);
                return true;
            };
            // A leaf line in the color of the node at `path`.
            auto leaf = [&](const std::string& path, const std::string& text) {
                const bool pushed = pushMark(MarkFor(path, applied));
                ImGui::BulletText("%s", text.c_str());
                if (pushed) {
                    ImGui::PopStyleColor();
                }
            };
            // A value as the tree shows it: strings without quotes, anything else as JSON.
            auto valueText = [](const nlohmann::json& value) {
                return value.is_string() ? value.get<std::string>() : value.dump();
            };

            // Entities, then their fields. Counters and lists open one more level.
            for (const auto& [id, entity] : run.State().items()) {
                const bool entityPushed = pushMark(MarkFor(id, applied));
                const bool entityOpen = ImGui::TreeNodeEx(id.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s (%s)",
                                                          id.c_str(), entity.at("type").get<std::string>().c_str());
                if (entityPushed) {
                    ImGui::PopStyleColor();
                }
                if (!entityOpen) {
                    continue;
                }
                for (const auto& [field, value] : entity.at("fields").items()) {
                    const std::string path = id + "." + field;
                    if (!value.is_object() && !value.is_array()) {
                        leaf(path, field + ": " + valueText(value));
                        continue;
                    }
                    const bool fieldPushed = pushMark(MarkFor(path, applied));
                    const bool fieldOpen = ImGui::TreeNodeEx(path.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)",
                                                             field.c_str(), value.size());
                    if (fieldPushed) {
                        ImGui::PopStyleColor();
                    }
                    if (!fieldOpen) {
                        continue;
                    }
                    if (value.is_object()) {
                        // A counter: each item has its own path.
                        for (const auto& [item, count] : value.items()) {
                            leaf(path + "." + item, item + ": " + count.dump());
                        }
                    } else {
                        // A list: its elements change with the field.
                        for (const nlohmann::json& element : value) {
                            leaf(path, valueText(element));
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 5: Write the Turn log window**

Create `src/app/ui/TurnLogWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "app/model/Inspect.h"
#include "context/Transcript.h"
#include "core/Text.h"

namespace morpheus::app {

namespace {

constexpr std::size_t TITLE_INPUT_CHARS = 60;    // The input shown in a turn's header, in bytes.
constexpr float RESOLVE_JSON_LINES = 12.0f;      // Height of the raw resolve output, in lines.
constexpr const char* CALLS[] = {"lookup", "resolve", "narrate"}; // Model calls, in turn order.

} // namespace

void TurnLogWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(TURN_LOG_WINDOW)) {
        const std::vector<core::TurnRecord>& history = controller.Run().History();
        if (history.empty()) {
            ImGui::TextDisabled("No turns yet.");
        }
        // Newest first; the latest starts open.
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const core::TurnRecord& turn = *it;
            ImGui::PushID(turn.number);
            if (it == history.rbegin()) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            // "###header" keeps the header's ID stable while its label changes (a regenerate adds an attempt).
            const std::string attempt = turn.attempt > 0 ? std::format(" (attempt {})", turn.attempt + 1) : std::string();
            const std::string title = std::format("Turn {}{}: {}###header", turn.number, attempt,
                                                  core::CutUtf8(turn.input, TITLE_INPUT_CHARS));
            if (ImGui::CollapsingHeader(title.c_str())) {
                // What the player asked and what the model made of it.
                ImGui::TextWrapped("Input: %s", turn.input.c_str());
                const nlohmann::json intent =
                    turn.resolve.is_object() ? turn.resolve.value("intent", nlohmann::json()) : nlohmann::json();
                ImGui::TextWrapped("Intent: %s", intent.is_string() ? intent.get<std::string>().c_str() : "(none)");

                if (!turn.checks.empty()) {
                    ImGui::SeparatorText("Checks");
                    for (const core::CheckOutcome& check : turn.checks) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s (stakes: %s)", CheckText(check).c_str(), check.stakes.c_str());
                    }
                }
                if (!turn.applied.empty()) {
                    ImGui::SeparatorText("Applied");
                    for (const core::AppliedChange& applied : turn.applied) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s - %s", context::RenderChangePart(applied).c_str(),
                                           applied.change.reason.c_str());
                    }
                }
                if (!turn.rejected.empty()) {
                    ImGui::SeparatorText("Rejected");
                    for (const core::RejectedChange& rejected : turn.rejected) {
                        TextWrappedColored(ERROR_COLOR, std::format("{} - {}", rejected.raw.dump(), rejected.reason));
                    }
                }
                if (!turn.notes.empty()) {
                    ImGui::SeparatorText("Notes");
                    for (const std::string& note : turn.notes) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s", note.c_str());
                    }
                }

                // Assisted mode: what the lookup call asked for, and how much of it reached the prompt.
                const nlohmann::json& timings = turn.timings;
                if (timings.is_object() && timings.contains("lookup")) {
                    ImGui::SeparatorText("Lookups");
                    const nlohmann::json& lookup = timings.at("lookup");
                    for (const nlohmann::json& request : lookup.value("requests", nlohmann::json::array())) {
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s \"%s\": %zu found, %zu reached the prompt",
                                           request.at("source").get<std::string>().c_str(),
                                           request.at("query").get<std::string>().c_str(), request.at("added").size(),
                                           request.value("kept", nlohmann::json::array()).size());
                    }
                    if (lookup.contains("error")) {
                        TextWrappedColored(ERROR_COLOR,
                                           "Unreadable lookup output: " + lookup.at("error").get<std::string>());
                    }
                }

                // Each model call's numbers.
                ImGui::SeparatorText("Calls");
                if (ImGui::BeginTable("calls", 6,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const char* header : {"Call", "Prompt", "Reused", "Generated", "Prompt ms", "Generate ms"}) {
                        ImGui::TableSetupColumn(header);
                    }
                    ImGui::TableHeadersRow();
                    for (const char* call : CALLS) {
                        const std::optional<CallStats> stats = CallStatsOf(timings, call);
                        if (!stats) {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(call);
                        if (stats->stoppedAtLimit) {
                            ImGui::SameLine();
                            ImGui::TextColored(WARNING_COLOR, "(hit max tokens)");
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", stats->promptTokens);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", stats->reused);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", stats->generated);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", stats->promptMs);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", stats->generateMs);
                    }
                    ImGui::EndTable();
                }
                if (timings.is_object() && timings.contains("resolve") && timings.at("resolve").contains("error")) {
                    TextWrappedColored(ERROR_COLOR, "Unreadable resolve output: " +
                                                        timings.at("resolve").at("error").get<std::string>());
                }

                // The resolve output exactly as the model wrote it.
                if (ImGui::TreeNode("Resolve output (JSON)")) {
                    std::string json = turn.resolve.dump(2);
                    ImGui::InputTextMultiline("##resolve", &json,
                                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * RESOLVE_JSON_LINES),
                                              ImGuiInputTextFlags_ReadOnly);
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 6: Write the Context window**

Create `src/app/ui/ContextWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <cfloat>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

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

            // The prompt itself: one section per message, or the whole text to copy.
            ImGui::SeparatorText("Prompt");
            if (ImGui::RadioButton("Sections", !showRaw)) {
                showRaw = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Raw", showRaw)) {
                showRaw = true;
            }
            if (showRaw) {
                std::string text = RawPrompt(assembly->prompt);
                ImGui::InputTextMultiline("##raw", &text, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
            } else {
                if (ImGui::BeginChild("sections")) {
                    for (const PromptSection& section : PromptSections(assembly->prompt)) {
                        if (ImGui::CollapsingHeader(section.title.c_str())) {
                            ImGui::TextWrapped("%s", section.text.c_str());
                        }
                    }
                }
                ImGui::EndChild();
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 7: Write the Memory window**

Create `src/app/ui/MemoryWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <format>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/model/Inspect.h"

namespace morpheus::app {

void MemoryWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(MEMORY_WINDOW)) {
        const RunModel& run = controller.Run();
        if (!run.Open()) {
            ImGui::TextDisabled("No run open.");
        } else {
            const turn::MemoryView& memory = run.Memory();

            // The story summary (L2): folded turn ranges, oldest first.
            ImGui::SeparatorText(std::format("Story summary ({})", memory.summaries.size()).c_str());
            if (memory.summaries.empty()) {
                ImGui::TextDisabled("Nothing folded yet.");
            }
            for (const core::Summary& summary : memory.summaries) {
                if (ImGui::TreeNode(std::format("Turns {}-{}", summary.fromTurn, summary.toTurn).c_str())) {
                    ImGui::TextWrapped("%s", summary.text.c_str());
                    ImGui::TreePop();
                }
            }

            // The model's notes (assisted mode).
            ImGui::SeparatorText(std::format("Notes ({})", memory.notes.size()).c_str());
            if (memory.notes.empty()) {
                ImGui::TextDisabled("No notes. The model writes them in assisted memory mode.");
            }
            for (const core::Note& note : memory.notes) {
                ImGui::Bullet();
                ImGui::TextWrapped("Turn %d: %s", note.turn, note.text.c_str());
            }

            // What retrieval put in the latest turn's prompt.
            ImGui::SeparatorText("Retrieved for the latest turn");
            const core::TurnRecord* pLatest = run.LatestTurn();
            const std::vector<RetrievalRow> rows =
                pLatest ? RetrievalRows(pLatest->timings, run.History(), memory.notes) : std::vector<RetrievalRow>();
            if (rows.empty()) {
                ImGui::TextDisabled("Nothing retrieved.");
            } else if (ImGui::BeginTable("hits", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const RetrievalRow& row : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.source.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.ref.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", row.score);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", row.text.c_str());
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 8: Write the Settings window**

Create `src/app/ui/SettingsWindow.cpp`:

```cpp
#include "app/ui/Windows.h"

#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/Errors.h"

namespace morpheus::app {

namespace {

constexpr const char* MEMORY_MODES[] = {"World default", "engine", "assisted"}; // Combo items, in this order.

} // namespace

void SettingsWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(SETTINGS_WINDOW)) {
        // The edit starts from the file: on the first draw, and again after Revert.
        if (!_edit) {
            try {
                _edit = ReadSettingsEdit(controller.Paths().settingsFile);
                _problem.clear();
            } catch (const core::DataError& error) {
                _problem = error.what(); // Edited outside the app into something unreadable: shown below.
            }
        }
        if (_edit) {
            SettingsEdit& edit = *_edit;

            ImGui::SeparatorText("Models");
            ImGui::InputText("Chat model", &edit.chatModel);
            ImGui::InputText("Embedding model", &edit.embeddingModel);
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            ImGui::InputInt("GPU layers", &edit.gpuLayers);
            ImGui::SetItemTooltip("-1 puts every layer on the GPU.");

            // One call type's sampling, under its own ID scope (the labels repeat).
            auto samplingFields = [](const char* id, core::SamplingSettings& sampling) {
                ImGui::PushID(id);
                ImGui::InputDouble("Temperature", &sampling.temperature, 0.05, 0.1, "%.2f");
                ImGui::InputInt("Top K", &sampling.topK);
                ImGui::InputDouble("Top P", &sampling.topP, 0.01, 0.05, "%.2f");
                ImGui::PopID();
            };
            ImGui::SeparatorText("Sampling");
            ImGui::TextUnformatted("Resolve (proposes the turn's changes)");
            samplingFields("resolve", edit.resolveSampling);
            ImGui::TextUnformatted("Narrate (writes the prose)");
            samplingFields("narrate", edit.narrateSampling);

            ImGui::SeparatorText("Memory");
            int mode = !edit.memoryMode ? 0 : *edit.memoryMode == core::MemoryMode::Engine ? 1 : 2;
            if (ImGui::Combo("Memory mode", &mode, MEMORY_MODES, IM_ARRAYSIZE(MEMORY_MODES))) {
                edit.memoryMode = mode == 0 ? std::nullopt
                                            : std::optional<core::MemoryMode>(mode == 1 ? core::MemoryMode::Engine
                                                                                        : core::MemoryMode::Assisted);
            }

            // The budget is shown, not edited here: it is tuned in settings.json.
            ImGui::SeparatorText("Budget (edit settings.json)");
            const core::BudgetSettings& budget = controller.Engine().CurrentSettings().budget;
            const std::pair<const char*, int> rows[] = {
                {"resolve_max_tokens", budget.resolveMaxTokens}, {"narrate_max_tokens", budget.narrateMaxTokens},
                {"narrate_allowance", budget.narrateAllowance},  {"tail_allowance", budget.tailAllowance},
                {"engine_rules_cap", budget.engineRulesCap},     {"world_cap", budget.worldCap},
                {"summary_cap", budget.summaryCap},              {"retrieval_cap", budget.retrievalCap}};
            for (const auto& [name, value] : rows) {
                ImGui::Text("%s: %d", name, value);
            }

            // Apply restarts the engine, so it waits while a command runs or models load.
            ImGui::Separator();
            ImGui::BeginDisabled(!controller.CanApplySettings());
            if (ImGui::Button("Apply")) {
                const std::string refused = controller.ApplySettings(edit);
                _problem = refused.empty() ? std::string() : "Not applied: " + refused;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool revert = ImGui::Button("Revert");
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. New model paths, context size or "
                                "GPU layers reload the models.");
            if (revert) {
                _edit.reset(); // Read from the file again on the next draw.
                _problem.clear();
            }
        }
        if (!_problem.empty()) {
            TextWrappedColored(ERROR_COLOR, _problem);
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[ui]"`
Expected: all three UI tests pass. The first two now draw every window too. Then `ctest --preset tests`: everything passes.

If the inspectors test's `rejected` check fails, read the reason the engine gives for `player.wings`. The mini world's `character` type has no `wings` field, so the change must be rejected. Report it rather than changing the fixture.

- [ ] **Step 10: Commit**

```bash
git add src/app/CMakeLists.txt src/app/ui/Windows.h src/app/ui/Ui.h src/app/ui/Ui.cpp src/app/ui/StateWindow.cpp src/app/ui/TurnLogWindow.cpp src/app/ui/ContextWindow.cpp src/app/ui/MemoryWindow.cpp src/app/ui/SettingsWindow.cpp tests/app/UiTests.cpp
git commit -m "Add the State, Turn log, Context, Memory and Settings windows" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
