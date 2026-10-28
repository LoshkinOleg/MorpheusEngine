#include "app/ui/Ui.h"

#include <format>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (the default layout) and BeginViewportSideBar (the status bar).

namespace morpheus::app {

void TextWrappedColored(const ImVec4& color, std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Ui::Draw(AppController& controller)
{
    ImGuiViewport* pViewport = ImGui::GetMainViewport();

    // The status bar along the bottom: what the engine and the run are doing. It is drawn before the dock space,
    // which then fills the rest of the viewport.
    std::string status;
    switch (controller.Engine().CurrentStatus()) {
    case EngineHost::Status::Stopped:
        status = "Engine stopped";
        break;
    case EngineHost::Status::Loading:
        status = "Loading models...";
        break;
    case EngineHost::Status::Failed:
        status = "Engine failed: " + controller.Engine().Failure() + " (fix it in Settings and apply)";
        break;
    case EngineHost::Status::Ready:
        switch (controller.Run().CurrentBusy()) {
        case RunModel::Busy::None:
            status = controller.Run().Open() ? std::format("Ready - {}, {} turns", controller.Run().Meta().worldId,
                                                           controller.Run().History().size())
                                             : std::string("Ready - no run open");
            break;
        case RunModel::Busy::Opening:
            status = "Opening the run...";
            break;
        case RunModel::Busy::Turn:
            status = "Playing the turn...";
            break;
        case RunModel::Busy::Regenerate:
            status = "Playing the turn again...";
            break;
        case RunModel::Busy::Undo:
            status = "Undoing the turn...";
            break;
        }
        break;
    }
    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", pViewport, ImGuiDir_Down, ImGui::GetFrameHeight(), barFlags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted(status.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    // The dock space. Without a saved layout (first launch, or imgui.ini deleted), the windows are docked into the
    // default one: runs and settings on the left, the story in the middle, state and memory on the right, and the
    // turn log and context below the story.
    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, pViewport->WorkSize);
        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGui::DockBuilderDockWindow(RUNS_WINDOW, left);
        ImGui::DockBuilderDockWindow(SETTINGS_WINDOW, left);
        ImGui::DockBuilderDockWindow(PLAY_WINDOW, center);
        ImGui::DockBuilderDockWindow(STATE_WINDOW, right);
        ImGui::DockBuilderDockWindow(MEMORY_WINDOW, right);
        ImGui::DockBuilderDockWindow(TURN_LOG_WINDOW, bottom);
        ImGui::DockBuilderDockWindow(CONTEXT_WINDOW, bottom);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, pViewport);

    _runs.Draw(controller);
    _play.Draw(controller);
    _state.Draw(controller);
    _turnLog.Draw(controller);
    _context.Draw(controller);
    _memory.Draw(controller);
    _settings.Draw(controller);
}

} // namespace morpheus::app
