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
