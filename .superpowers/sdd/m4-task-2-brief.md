### Task 2: Dear ImGui's docking branch, drawable headless

The current ImGui fork has no docking. This task fetches upstream's docking branch and defines two targets: `imgui_core` (no backend, built for tests and the app) and `imgui_sdl3` (the SDL3 and SDL_Renderer backends, app only). It sets our ImGui configuration with a throwing `IM_ASSERT`, and proves a headless context draws a docked window. The prototype app keeps building against the new targets until Task 11 replaces it.

**Files:**
- Modify: `CMakeLists.txt` (the ImGui fetch, the prototype's link line), `CMakePresets.json` (the `tests` description), `tests/CMakeLists.txt`
- Create: `src/app/ui/ImGuiConfig.h`, `tests/app/HeadlessImGui.h`, `tests/app/ImGuiHeadlessTests.cpp`

**Interfaces:**
- Produces:
  - The CMake targets `imgui_core` and `imgui_sdl3`.
  - `IM_ASSERT` throws `std::logic_error`.
  - `test::HeadlessImGui` with `template <typename Draw> void Frame(Draw&& draw)`.

- [ ] **Step 1: Write the failing test**

Create `tests/app/HeadlessImGui.h`:

```cpp
#pragma once

#include <memory>

#include <imgui.h>

namespace morpheus::test {

// An ImGui context with no platform or renderer backend, for drawing windows in tests. The font atlas is built up
// front, as a renderer backend would; frames are laid out but never turned into pixels. Docking is on, like the app.
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
        // Without a renderer backend, the atlas must be built before the first frame.
        unsigned char* pPixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pPixels, &width, &height);
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
```

Create `tests/app/ImGuiHeadlessTests.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset tests`
Expected: a compile error: `imgui.h` is not found, because the `tests` preset fetches no ImGui yet.

- [ ] **Step 3: Write the ImGui configuration**

Create `src/app/ui/ImGuiConfig.h`:

```cpp
#pragma once

// Dear ImGui's build configuration (IMGUI_USER_CONFIG), set by the imgui_core target for everything that compiles
// or includes ImGui.

#include <stdexcept>
#include <string>

// A failed ImGui assertion is a UI bug (an unbalanced Begin/End, a broken ID stack). Throwing fails loud with the
// expression and location, and lets a headless test report it instead of aborting the test run.
#define IM_ASSERT(expr)                                                                                               \
    ((expr) ? static_cast<void>(0)                                                                                    \
            : throw std::logic_error(std::string("ImGui assertion failed: ") + #expr + " [" + __FILE__ + ":" +       \
                                     std::to_string(__LINE__) + "]"))

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
```

- [ ] **Step 4: Fetch upstream ImGui and define its targets**

In `CMakeLists.txt`, replace the ImGui block inside `if(MORPHEUS_BUILD_APP)` (the `FetchContent_Declare(imgui ... cmake-ready-imgui-sdl3 ...)` and its `FetchContent_MakeAvailable(imgui)`), so that block only fetches SDL3. Then add, directly after that `if(MORPHEUS_BUILD_APP) ... endif()`:

```cmake
if(MORPHEUS_BUILD_APP OR MORPHEUS_BUILD_TESTS)
    # Dear ImGui, docking branch: the app's UI, and the tests draw every window headless. The repository has no
    # CMake build, so its targets are defined here, like SQLite's.
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        b61e56346a92cfcaf1f43a545ca37b0b32239654       # commit SHA to get, v1.92.8-docking
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    )
    FetchContent_MakeAvailable(imgui)

    # Core ImGui and its std::string input helpers, without a platform or renderer backend.
    add_library(imgui_core STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    )
    target_include_directories(imgui_core SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp)
    # Our configuration (a throwing IM_ASSERT, no obsolete functions) for every target that includes ImGui.
    target_compile_definitions(imgui_core PUBLIC IMGUI_USER_CONFIG="${PROJECT_SOURCE_DIR}/src/app/ui/ImGuiConfig.h")
    target_compile_features(imgui_core PUBLIC cxx_std_17)

    if(MORPHEUS_BUILD_APP)
        # The SDL3 platform backend and the SDL_Renderer backend, for the app.
        add_library(imgui_sdl3 STATIC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        )
        target_include_directories(imgui_sdl3 SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
        target_link_libraries(imgui_sdl3 PUBLIC imgui_core SDL3::SDL3)
    endif()
endif()
```

In the prototype app's `target_link_libraries(morpheus PRIVATE ...)`, replace `imgui::imgui` with `imgui_sdl3`.

In `tests/CMakeLists.txt`, add `imgui_core` to the `target_link_libraries(morpheus_tests PRIVATE ...)` line.

In `CMakePresets.json`, change the `tests` preset's `displayName` to `"Engine libraries + tests only (no llama.cpp or SDL3)"`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[imgui]"`
Expected: both tests pass. Then run `ctest --preset tests`: everything passes.

If `ImGui::End()` does not throw, read `ImGui::End` in `build/tests/_deps/imgui-src/imgui.cpp`. In 1.92.8 an extra `End` reaches `IM_ASSERT_USER_ERROR`, which calls `IM_ASSERT` while `io.ConfigErrorRecoveryEnableAssert` is true (the default). Report what you find rather than changing the test's intent.

- [ ] **Step 6: Check that the prototype app still builds**

Run: `cmake --preset dev && cmake --build --preset dev --target morpheus`
Expected: `build/dev/bin/Debug/morpheus.exe` links against `imgui_sdl3`. The prototype is not `/WX`, and Task 11 deletes it, so do not edit `src/main.cpp`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt CMakePresets.json tests/CMakeLists.txt src/app/ui/ImGuiConfig.h tests/app/HeadlessImGui.h tests/app/ImGuiHeadlessTests.cpp
git commit -m "Fetch Dear ImGui's docking branch and draw it headless in the tests" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
