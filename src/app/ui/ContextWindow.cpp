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
