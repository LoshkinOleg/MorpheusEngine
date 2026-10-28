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
