#pragma once

#include <memory>

#include <imgui.h>

namespace morpheus::test {

// An ImGui context with no platform or renderer backend, for drawing windows in tests. It claims texture support, as
// a modern renderer backend does, so the font atlas builds itself on demand; frames are laid out but never turned
// into pixels. Docking is on, like the app.
class HeadlessImGui {
private:
    struct ContextDeleter {
        void operator()(ImGuiContext* pContext) const
        {
            ImGui::DestroyContext(pContext);
        }
    };

    std::unique_ptr<ImGuiContext, ContextDeleter> _pContext = nullptr;

public:
    HeadlessImGui()
        : _pContext(ImGui::CreateContext())
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1600.0f, 900.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr; // No imgui.ini: every test starts from the default layout.
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // With IMGUI_DISABLE_OBSOLETE_FUNCTIONS the legacy GetTexDataAsRGBA32 path does not exist, so the context claims
        // texture support and the atlas builds on demand. Textures are never uploaded headless.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    // Draws one frame: `draw` runs between NewFrame and Render.
    template <typename Draw>
    void Frame(Draw&& draw)
    {
        ImGui::NewFrame();
        draw();
        ImGui::Render();
    }
};

} // namespace morpheus::test
