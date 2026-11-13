cmake_minimum_required(VERSION 3.25)  # 3.25+ for FetchContent SYSTEM (keeps dependency headers out of /W4 /WX)

project(MorpheusEngine
    VERSION 1 # Major only notation: 1, 2, 3, 56, 103, etc.
    LANGUAGES C CXX # C is needed for the SQLite amalgamation.
)

# ---- Global settings ----------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF) # Enforce strict standard syntax, no compiler specific lingo.

# Put all binaries in one place - handy on Windows for DLL discovery. Only when Morpheus is the top-level project:
# a host project that includes Morpheus decides where binaries go.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win
endif()

# ---- Options ------------------------------------------------------------
# The app, the tests, llama.cpp and warnings as errors default to on only when Morpheus is the top-level project. A
# host project that includes Morpheus (FetchContent, add_subdirectory) gets the engine libraries alone, and a warning
# from the host's compiler in Morpheus's code does not fail the host's build.
option(MORPHEUS_BUILD_APP "Build the morpheus app (needs MORPHEUS_BUILD_LLAMA; fetches SDL3)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_BUILD_TESTS "Build morpheus_tests (fetches Catch2, and ImGui's core for the headless UI tests)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend, morpheus_probe and morpheus_play (fetches and builds llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_WARNINGS_AS_ERRORS "Treat warnings as errors on MorpheusEngine's own targets (/WX or -Werror)" ${PROJECT_IS_TOP_LEVEL})
option(MORPHEUS_ASAN "Instrument every target with AddressSanitizer (MSVC only)" OFF)

# llama.cpp's GPU backend. Checked on every configure, with or without MORPHEUS_BUILD_LLAMA, so that a misspelt value
# never passes silently.
set(MORPHEUS_GPU_BACKENDS Vulkan CUDA CPU)
set(MORPHEUS_GPU_BACKEND "Vulkan" CACHE STRING
    "llama.cpp's GPU backend: Vulkan (needs the Vulkan SDK), CUDA (needs the CUDA toolkit) or CPU")
set_property(CACHE MORPHEUS_GPU_BACKEND PROPERTY STRINGS ${MORPHEUS_GPU_BACKENDS})
if(NOT MORPHEUS_GPU_BACKEND IN_LIST MORPHEUS_GPU_BACKENDS)
    list(JOIN MORPHEUS_GPU_BACKENDS ", " MORPHEUS_GPU_BACKEND_NAMES)
    message(FATAL_ERROR "MORPHEUS_GPU_BACKEND is '${MORPHEUS_GPU_BACKEND}'; it must be one of: ${MORPHEUS_GPU_BACKEND_NAMES}.")
endif()

if(MORPHEUS_BUILD_APP AND NOT MORPHEUS_BUILD_LLAMA)
    message(FATAL_ERROR "MORPHEUS_BUILD_APP needs MORPHEUS_BUILD_LLAMA.")
endif()

# AddressSanitizer is applied to every target, dependencies included: MSVC refuses to link instrumented and
# non-instrumented objects that share STL containers (LNK2038 'annotate_vector' mismatch).
if(MORPHEUS_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "MORPHEUS_ASAN is only wired up for MSVC.")
    endif()
    add_compile_options(/fsanitize=address)
    # MSVC's default Debug runtime checks (/RTC1) cannot be combined with /fsanitize=address.
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# Strict settings for MorpheusEngine's own targets. Never applied to fetched dependencies. The warnings are always on;
# they fail the build only with MORPHEUS_WARNINGS_AS_ERRORS (on by default at top level, off when vendored).
function(morpheus_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        # /W4 warning level, /permissive- standard compliance, /Zc:preprocessor standard preprocessor,
        # /utf-8 source and execution character sets.
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    # Warnings as errors through the CMake property (/WX or -Werror), so that the option holds even when a host sets
    # CMAKE_COMPILE_WARNING_AS_ERROR, and `cmake --build --compile-no-warning-as-error` is respected.
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ${MORPHEUS_WARNINGS_AS_ERRORS})
endfunction()

# ---- Dependencies (FetchContent) ---------------------------------------
include(FetchContent)

# nlohmann (engine libraries and app). A host project that already provides it (its own FetchContent or find_package)
# keeps its copy, and Morpheus links the same target.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann                                                      # prefix for CMake variables
        GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
        GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Treat its headers as external (no /W4 /WX)
    )
    FetchContent_MakeAvailable(nlohmann)
endif()

# SQLite amalgamation (store). The archive has no CMakeLists.txt, so the library target is defined here, under a name
# of Morpheus's own so that it never meets a host project's sqlite3 target.
FetchContent_Declare(
    morpheus_sqlite
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip   # SQLite 3.50.4
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(morpheus_sqlite)
add_library(morpheus_sqlite3 STATIC ${morpheus_sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(morpheus_sqlite3 SYSTEM PUBLIC ${morpheus_sqlite_SOURCE_DIR})
target_compile_definitions(morpheus_sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
# Third-party code: never warnings as errors, even when a host sets CMAKE_COMPILE_WARNING_AS_ERROR.
set_target_properties(morpheus_sqlite3 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

if(MORPHEUS_BUILD_LLAMA)
    # llama.cpp's own backend switches follow MORPHEUS_GPU_BACKEND: GGML_VULKAN only for Vulkan, GGML_CUDA only for
    # CUDA, neither for CPU. FORCE so that changing the choice reconfigures the fetched llama.cpp.
    set(MORPHEUS_GGML_VULKAN OFF)
    set(MORPHEUS_GGML_CUDA OFF)
    if(MORPHEUS_GPU_BACKEND STREQUAL "Vulkan")
        set(MORPHEUS_GGML_VULKAN ON)
    elseif(MORPHEUS_GPU_BACKEND STREQUAL "CUDA")
        set(MORPHEUS_GGML_CUDA ON)
    endif()
    set(GGML_VULKAN ${MORPHEUS_GGML_VULKAN} CACHE BOOL "" FORCE)
    set(GGML_CUDA ${MORPHEUS_GGML_CUDA} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git      # repo
        GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Its headers are external: no /W4 /WX
    )
    FetchContent_MakeAvailable(llama)
endif()

if(MORPHEUS_BUILD_APP)
    # SDL3
    FetchContent_Declare(
        sdl3                                                          # prefix for CMake variables
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git          # repo
        GIT_TAG        8e37db5e797b6167f3a00d697d816a684bd259c7       # commit SHA to get, SDL3 3.4.10
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM                                                        # Its headers are external: no /W4 /WX
    )
    FetchContent_MakeAvailable(sdl3)
endif()

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
    if(MSVC)
        # ImGui's implicitly noexcept destructors (ImGuiContext, ImDrawListSharedData, ImFontAtlas) contain IM_ASSERT,
        # which our configuration makes throwing: silence C4297 for them.
        target_compile_options(imgui_core PRIVATE /wd4297)
    endif()

    if(MORPHEUS_BUILD_APP)
        # The SDL3 platform backend and the SDL_Renderer backend, for the app.
        add_library(imgui_sdl3 STATIC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        )
        target_include_directories(imgui_sdl3 SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
        target_link_libraries(imgui_sdl3 PUBLIC imgui_core SDL3::SDL3)
        if(MSVC)
            # Same as imgui_core: ImGui's noexcept destructors contain the throwing IM_ASSERT.
            target_compile_options(imgui_sdl3 PRIVATE /wd4297)
        endif()
    endif()
endif()

if(MORPHEUS_BUILD_TESTS)
    # Catch2
    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        08092139210881f01a34e60c5f2fd6afa0ea4024       # commit SHA to get, v3.16.1
        GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
        SYSTEM
    )
    FetchContent_MakeAvailable(catch2)
endif()

# ---- Engine libraries ---------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/store)
add_subdirectory(src/inference)
add_subdirectory(src/context)
add_subdirectory(src/turn)
if(MORPHEUS_BUILD_LLAMA)
    add_subdirectory(src/llama)
    add_subdirectory(tools/probe)
    add_subdirectory(tools/play)
endif()

# ---- App ---------------------------------------------------------------
# The model and UI libraries build with the tests too (ImGui is fetched for both); the executable needs the app.
if(MORPHEUS_BUILD_APP OR MORPHEUS_BUILD_TESTS)
    add_subdirectory(src/app)
endif()

if(MORPHEUS_BUILD_APP AND PROJECT_IS_TOP_LEVEL)
    # Visual Studio opens the solution with the app as the startup project. A host's solution keeps its own.
    set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT morpheus)
endif()

# ---- Tests --------------------------------------------------------------
if(MORPHEUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
