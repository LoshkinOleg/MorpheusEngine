#include "app/ui/Windows.h"

#include <cfloat>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/model/ModelSeat.h"

namespace morpheus::app {

namespace {

constexpr float REPLY_LINES = 8.0f;    // Height of the reply box, in lines of text.
constexpr float THINKING_LINES = 4.0f; // Height of the thinking box, in lines of text.

} // namespace

void ModelSeatWindow::Draw(AppController& controller)
{
    if (ImGui::Begin(MODEL_SEAT_WINDOW)) {
        ModelSeat& seat = controller.Seat();

        // The switch: while it is off, the model answers every call as usual.
        bool enabled = seat.Enabled();
        if (ImGui::Checkbox("Answer as the model", &enabled)) {
            seat.SetEnabled(enabled);
        }

        // Who answers each kind of call: the owner (Human), the owner from the model's draft (Draft), or the model.
        if (ImGui::BeginTable("modes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* header : {"Call", "Human", "Draft", "Model"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (const SeatLabel label : SEAT_LABELS) {
                ImGui::PushID(static_cast<int>(label));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(SeatLabelName(label).c_str());
                for (const SeatMode mode : {SeatMode::Human, SeatMode::Draft, SeatMode::Model}) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(mode));
                    if (ImGui::RadioButton("##mode", seat.ModeOf(label) == mode)) {
                        seat.SetMode(label, mode);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        const std::optional<SeatRequest> request = seat.Pending();
        if (!request) {
            ImGui::TextDisabled("%s", seat.Enabled() ? "No call waits for a reply." : "Off: the model answers every call.");
        } else {
            // A new call: the reply starts as the model's draft (Draft mode) or empty (Human mode).
            if (request->id != _requestId) {
                _requestId = request->id;
                _reply = request->draft ? request->draft->text : std::string();
                _thinking = request->draft ? request->draft->thinking : std::string();
                _check.reset();
            }
            const inference::GenerationSpec& spec = request->spec;

            // Which call waits, and the limits the model would be held to.
            ImGui::SeparatorText(std::format("The {} call waits for a reply", SeatLabelName(request->label)).c_str());
            if (spec.thinkingMaxTokens > 0) {
                ImGui::Text("At most %d answer tokens, after at most %d thinking tokens", spec.maxTokens,
                            spec.thinkingMaxTokens);
            } else {
                ImGui::Text("At most %d answer tokens", spec.maxTokens);
            }
            if (!seat.Enabled()) {
                TextWrappedColored(WARNING_COLOR, "The seat is off: this call still waits for a reply; later calls go "
                                                  "to the model.");
            }

            // The model's thinking (Draft mode, read-only) and the grammar, folded away until opened.
            if (request->draft && !request->draft->thinking.empty() && ImGui::CollapsingHeader("The model's thinking")) {
                ImGui::TextWrapped("%s", request->draft->thinking.c_str());
            }
            if (!spec.grammar.empty() && ImGui::CollapsingHeader("Grammar")) {
                ImGui::TextUnformatted(spec.grammar.c_str());
            }

            // The reply, and the thinking when the call may think. Edits stay in this window until Submit.
            const float padding = ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::TextUnformatted("Reply");
            if (ImGui::InputTextMultiline("##reply", &_reply,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * REPLY_LINES + padding))) {
                _check.reset(); // The last check was of another text.
            }
            if (spec.thinkingMaxTokens > 0) {
                ImGui::TextUnformatted("Thinking");
                ImGui::InputTextMultiline("##thinking", &_thinking,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * THINKING_LINES + padding));
            }

            // Check (only with a grammar), Submit (whether or not the reply fits), and Withdraw (always: it releases
            // the call without a reply).
            ImGui::BeginDisabled(spec.grammar.empty());
            const bool check = ImGui::Button("Check");
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool submit = ImGui::Button("Submit");
            ImGui::SameLine();
            const bool withdraw = ImGui::Button("Withdraw");
            if (_check) {
                ImGui::Text("%d of %d answer tokens", _check->tokens, spec.maxTokens);
                if (_check->problem.empty()) {
                    ImGui::TextUnformatted("Fits the grammar");
                } else {
                    TextWrappedColored(WARNING_COLOR, _check->problem);
                }
            }

            // The prompt, as the Context window draws it, in the rest of the window.
            ImGui::SeparatorText("Prompt");
            DrawPrompt(request->prompt, _showRaw);

            // Act after drawing, one action per frame, on the request drawn (by its id). A request a cancel ended
            // since Pending() is stale: Submit and Withdraw then return false and Check nullopt, and nothing is left
            // to do.
            if (withdraw) {
                const RunModel::Busy busy = controller.Run().CurrentBusy();
                if (busy == RunModel::Busy::Turn || busy == RunModel::Busy::Regenerate) {
                    // A running turn's call (a fold of its fit loop, or of the maintenance before it, included):
                    // cancel the command, as the Play window's Cancel does, so the turn closes with TurnCancelled.
                    controller.Cancel();
                } else {
                    // Between-turn maintenance: release the fold or condense at the seat. The maintenance stops as a
                    // preempted one does, and the next turn folds what it needs.
                    seat.Withdraw(request->id);
                }
            } else if (submit) {
                seat.Submit(request->id, std::exchange(_reply, std::string()), std::exchange(_thinking, std::string()));
                _check.reset();
            } else if (check) {
                _check = seat.Check(request->id, _reply);
            }
        }
    }
    ImGui::End();
}

} // namespace morpheus::app
