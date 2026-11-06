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

            // What retrieval put in the latest turn's prompt, why each hit matched, and the scene it searched with.
            ImGui::SeparatorText("Retrieved for the latest turn");
            const core::TurnRecord* pLatest = run.LatestTurn();
            // Values joined with `separator`, for the scene line and a hit's reasons.
            auto joined = [](const std::vector<std::string>& values, const char* separator) {
                std::string text;
                for (const std::string& value : values) {
                    text += (text.empty() ? "" : separator) + value;
                }
                return text;
            };
            const std::vector<std::string> scene =
                pLatest ? SceneValues(pLatest->timings) : std::vector<std::string>();
            if (!scene.empty()) {
                ImGui::TextWrapped("Scene: %s", joined(scene, "; ").c_str());
            }
            const std::vector<RetrievalRow> rows =
                pLatest ? RetrievalRows(pLatest->timings, run.History(), memory.notes) : std::vector<RetrievalRow>();
            if (rows.empty()) {
                ImGui::TextDisabled("Nothing retrieved.");
            } else if (ImGui::BeginTable("hits", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Reasons", ImGuiTableColumnFlags_WidthFixed);
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
                    // Similarity alone shows a dash.
                    ImGui::TableNextColumn();
                    if (row.reasons.empty()) {
                        ImGui::TextDisabled("-");
                    } else {
                        ImGui::TextUnformatted(joined(row.reasons, ", ").c_str());
                    }
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
