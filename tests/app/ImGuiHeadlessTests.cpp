#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/HeadlessImGui.h"

using namespace morpheus;

TEST_CASE("ImGui draws a docked window headless", "[imgui]")
{
    test::HeadlessImGui imgui;
    // Two frames: the first creates the dock node and the window, the second draws them settled.
    for (int frame = 0; frame < 2; ++frame) {
        imgui.Frame([] {
            ImGui::DockSpaceOverViewport();
            ImGui::Begin("Window");
            ImGui::TextUnformatted("An em dash: \xE2\x80\x94");
            std::string text = "editable";
            ImGui::InputText("Input", &text);
            ImGui::End();
        });
    }
    CHECK(ImGui::GetDrawData()->Valid);
    CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0);
}

TEST_CASE("A failed ImGui assertion throws instead of aborting", "[imgui]")
{
    test::HeadlessImGui imgui;
    ImGui::NewFrame();
    CHECK_THROWS_AS(ImGui::End(), std::logic_error); // End without Begin.
    ImGui::EndFrame();
}
