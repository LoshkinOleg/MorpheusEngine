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
