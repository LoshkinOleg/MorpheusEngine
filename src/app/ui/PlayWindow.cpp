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
