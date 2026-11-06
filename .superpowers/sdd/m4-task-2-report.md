# M4 Task 2 report: Dear ImGui docking branch, headless

Status: DONE_WITH_CONCERNS. Commit 216e8af "Fetch Dear ImGui's docking branch and draw it headless in the tests".

## Implemented
- Root CMakeLists.txt: the fork fetch removed from the `MORPHEUS_BUILD_APP` block (it now fetches SDL3 only). A new
  `if(MORPHEUS_BUILD_APP OR MORPHEUS_BUILD_TESTS)` block fetches ocornut/imgui at b61e56346a92cfcaf1f43a545ca37b0b32239654 and
  defines `imgui_core` (imgui, draw, tables, widgets, imgui_stdlib; IMGUI_USER_CONFIG public define; SYSTEM includes; no
  /W4 /WX) and, under `MORPHEUS_BUILD_APP`, `imgui_sdl3` (SDL3 + SDLRenderer3 backends). The prototype `morpheus` links
  `imgui_sdl3` instead of `imgui::imgui`. `src/main.cpp` untouched.
- tests/CMakeLists.txt links `imgui_core`. CMakePresets.json `tests` displayName updated.
- New: src/app/ui/ImGuiConfig.h (throwing IM_ASSERT, IMGUI_DISABLE_OBSOLETE_FUNCTIONS), tests/app/HeadlessImGui.h,
  tests/app/ImGuiHeadlessTests.cpp.

## Deviation from the brief (one)
The brief's `HeadlessImGui` constructor calls `io.Fonts->GetTexDataAsRGBA32(...)`. At 1.92.8 that function does not
exist in our build: `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` (set by the brief's ImGuiConfig.h) removes it, giving
`error C2039: 'GetTexDataAsRGBA32': is not a member of 'ImFontAtlas'`. The 1.92 way: set
`io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures`; the atlas then builds on demand in NewFrame
(`GetDefaultFont` -> `ImFontAtlasBuildMain`), no pixels needed headless. I replaced the three lines with that flag and updated
the class comment. Test intent is unchanged. Task authors of later tasks should not call GetTexData* either.

## TDD evidence
RED: `cmake --preset tests; cmake --build --preset tests` ->
`ImGuiHeadlessTests.cpp(6,1): error C1083: Cannot open include file: 'imgui.h': No such file or directory`.
Second RED after CMake changes: `HeadlessImGui.h(35,19): error C2039: 'GetTexDataAsRGBA32': is not a member of 'ImFontAtlas'`.
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[imgui]"` -> `All tests passed (3 assertions in 2 test cases)`.
`ctest --preset tests` -> `100% tests passed out of 202`.
The End-without-Begin assertion does throw `std::logic_error` as predicted (via IM_ASSERT_USER_ERROR).
Dev prototype: `cmake --preset dev && cmake --build --preset dev --target morpheus` -> built
`build/dev/bin/Debug/morpheus.exe`, linked against imgui_sdl3 (pre-existing main.cpp warnings only).

## Files changed
CMakeLists.txt, CMakePresets.json, tests/CMakeLists.txt, src/app/ui/ImGuiConfig.h, tests/app/HeadlessImGui.h,
tests/app/ImGuiHeadlessTests.cpp.

## Self-review / concerns
- MSVC warning C4297 on imgui_core (dev build, and likely tests): `ImFontAtlas::~ImFontAtlas`, `ImDrawListSharedData::~ImDrawListSharedData`
  and `ImGui_ImplSDL3_ViewportData::~ImGui_ImplSDL3_ViewportData` contain IM_ASSERTs; with a throwing IM_ASSERT in implicitly
  noexcept destructors, C4297 fires (and a failing assert there would std::terminate). Warnings only, dependencies are not /WX.
  Could be silenced with `/wd4297` on the imgui targets if the owner wants a quiet build.
- The throwing-assert test makes ImGui print `[imgui-error]` lines to stderr (ImGui's error-recovery log); harmless noise.
- `ImGuiConfig.h` lives in src/app/ui but is not part of any morpheus_* library yet; it is referenced by absolute path via the define.
- Git warns LF->CRLF on the touched files (repo autocrlf=true); normal.

## Fix round
Changes: `/wd4297` (MSVC only, with a why-comment) as PRIVATE options on `imgui_core` and `imgui_sdl3`; ImGuiConfig.h comment
now says an assertion in an ImGui destructor ends the program through std::terminate (still loud); HeadlessImGui.h comment
explains the RendererHasTextures flag (no GetTexDataAsRGBA32 under IMGUI_DISABLE_OBSOLETE_FUNCTIONS, atlas builds on demand,
textures never uploaded); first test now records `ImGui::IsWindowDocked()` inside the frame and CHECKs `docked` and
`GetDrawData()->TotalVtxCount > 0`.
Docking: DockSpaceOverViewport() alone left IsWindowDocked() false (CHECK( docked ) FAILED, expansion false). I therefore
dock explicitly: `const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(); ImGui::SetNextWindowDockID(dockspaceId, ImGuiCond_Once);`
before Begin. Then: `All tests passed (5 assertions in 2 test cases)`.
Commands:
- `cmake --build --preset tests 2>&1 | grep -E "C4297|error|warning"` -> no output (after forced recompile of imgui_draw.cpp:
  `grep -cE "C4297|warning"` -> 0).
- dev preset, touched imgui_draw.cpp and imgui_impl_sdl3.cpp, `cmake --build --preset dev --target imgui_sdl3` grep for C4297|warning -> none.
- `./build/tests/bin/Debug/morpheus_tests.exe "[imgui]"` -> All tests passed (5 assertions in 2 test cases)
- `ctest --preset tests` -> 100% tests passed out of 202
