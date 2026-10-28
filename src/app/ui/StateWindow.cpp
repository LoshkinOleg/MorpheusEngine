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
