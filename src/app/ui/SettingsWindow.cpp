#include "app/ui/Windows.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/Errors.h"
#include "core/Settings.h"

namespace morpheus::app {

namespace {

constexpr const char* MEMORY_MODES[] = {"World default", "engine", "assisted"}; // Combo items, in this order.
// Combo items, in core::BackendKind's order.
constexpr const char* BACKENDS[] = {"llama.cpp, in-process (llama-cpp)", "llama-server over HTTP (llama-server)"};
constexpr const char* THINKING_TOOLTIP = "0 turns thinking off; otherwise at least %d. A model without a "
                                        "thinking mode needs 0.";

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

            // A model is picked from the models folder. The list is read when it opens, so files copied in while the
            // app runs show up. A path set by hand in settings.json stays as the current value until another is picked.
            auto modelCombo = [&](const char* label, std::string& model) {
                if (ImGui::BeginCombo(label, model.c_str())) {
                    if (ImGui::IsWindowAppearing()) {
                        _models = ListModels(controller.Paths().settingsFile);
                    }
                    if (_models.empty()) {
                        ImGui::TextDisabled("No .gguf files in %.*s/", static_cast<int>(MODELS_FOLDER.size()),
                                           MODELS_FOLDER.data());
                    }
                    for (const std::string& option : _models) {
                        const bool selected = option == model;
                        if (ImGui::Selectable(option.c_str(), selected)) {
                            model = option;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            };
            // The backend: models in-process, picked from the models folder, or two llama-server URLs and a slot.
            // Only the fields of the backend chosen show; the other's stay in settings.json.
            ImGui::SeparatorText("Models");
            int backend = edit.backend == core::BackendKind::LlamaCpp ? 0 : 1;
            if (ImGui::Combo("Backend", &backend, BACKENDS, IM_ARRAYSIZE(BACKENDS))) {
                edit.backend = backend == 0 ? core::BackendKind::LlamaCpp : core::BackendKind::LlamaServer;
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                modelCombo("Chat model", edit.chatModel);
                modelCombo("Embedding model", edit.embeddingModel);
            } else {
                ImGui::InputText("Chat server URL", &edit.chatUrl);
                ImGui::SetItemTooltip("http://host:port of the chat model's llama-server, e.g. http://127.0.0.1:8080.");
                ImGui::InputText("Embedding server URL", &edit.embeddingUrl);
                ImGui::SetItemTooltip("http://host:port of the embedding model's llama-server (started with "
                                      "--embedding), e.g. http://127.0.0.1:8081.");
                ImGui::InputInt("Server slot", &edit.serverSlot);
                ImGui::SetItemTooltip("The chat server's slot for Morpheus's calls (0 or more). Each slot keeps its "
                                      "own prompt cache.");
            }
            ImGui::InputInt("Context size", &edit.contextSize, 1024, 4096);
            if (edit.backend == core::BackendKind::LlamaServer) {
                ImGui::SetItemTooltip("What the budgets plan for. Each of the chat server's slots must hold at least "
                                      "this many tokens.");
            }
            if (edit.backend == core::BackendKind::LlamaCpp) {
                ImGui::InputInt("GPU layers", &edit.gpuLayers);
                ImGui::SetItemTooltip("-1 puts every layer on the GPU.");
            }
            ImGui::InputInt("Resolve thinking tokens", &edit.resolveThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
            ImGui::InputInt("Narrate thinking tokens", &edit.narrateThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);
            ImGui::InputInt("Review thinking tokens", &edit.reviewThinkingTokens, 256, 1024);
            ImGui::SetItemTooltip(THINKING_TOOLTIP, core::MIN_THINKING_TOKENS);

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

            // The narration review, and the compact transcript built from its digests.
            ImGui::SeparatorText("Narration review");
            ImGui::Checkbox("Narration check", &edit.narrationCheck);
            ImGui::SetItemTooltip("After each narration a review call checks it against the outcome; a narration "
                                  "that contradicts it is written once more.");
            ImGui::Checkbox("Compact transcript", &edit.compactTranscript);
            ImGui::SetItemTooltip("The review also writes a short digest of each turn, and older turns reach the "
                                  "prompt as their digest.");
            ImGui::BeginDisabled(!edit.compactTranscript);
            ImGui::InputInt("Prose turns", &edit.proseTurns);
            ImGui::SetItemTooltip("The newest turns that keep their full narration in the prompt. At least 1.");
            ImGui::EndDisabled();

            // The budget is shown, not edited here: it is tuned in settings.json.
            ImGui::SeparatorText("Budget (edit settings.json)");
            const core::BudgetSettings& budget = controller.Engine().CurrentSettings().budget;
            const std::pair<const char*, int> rows[] = {
                {"resolve_max_tokens", budget.resolveMaxTokens}, {"narrate_max_tokens", budget.narrateMaxTokens},
                {"review_max_tokens", budget.reviewMaxTokens},   {"narrate_allowance", budget.narrateAllowance},
                {"tail_allowance", budget.tailAllowance},        {"engine_rules_cap", budget.engineRulesCap},
                {"world_cap", budget.worldCap},                  {"summary_cap", budget.summaryCap},
                {"retrieval_cap", budget.retrievalCap}};
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
            ImGui::TextDisabled("Apply saves settings.json and restarts the engine. A new backend, model paths, server "
                                "URLs or slot, context size or GPU layers reload the backend.");
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
