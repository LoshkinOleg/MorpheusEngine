cmake_minimum_required(VERSION 3.25)  # 3.25+ for FetchContent SYSTEM (keeps dependency headers out of /W4 /WX)

project(MorpheusEngine
    VERSION 1 # Major only notation: 1, 2, 3, 56, 103, etc.
    LANGUAGES C CXX # C is needed for the SQLite amalgamation.
)

# ---- Global settings ----------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF) # Enforce strict standard syntax, no compiler specific lingo.

# Put all binaries in one place - handy on Windows for DLL discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win

# ---- Options ------------------------------------------------------------
option(MORPHEUS_BUILD_APP "Build the morpheus app (needs MORPHEUS_BUILD_LLAMA; fetches SDL3 and ImGui)" ON)
option(MORPHEUS_BUILD_TESTS "Build morpheus_tests (fetches Catch2)" ON)
option(MORPHEUS_ASAN "Instrument every target with AddressSanitizer (MSVC only)" OFF)
option(MORPHEUS_BUILD_LLAMA "Build the llama.cpp backend (fetches and builds llama.cpp)" ON)
option(MORPHEUS_VULKAN "Build llama.cpp with the Vulkan GPU backend (needs the Vulkan SDK)" ON)

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

# Strict settings for MorpheusEngine's own targets. Never applied to fetched dependencies.
function(morpheus_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        # /W4 warning level, /WX warnings are errors, /permissive- standard compliance,
        # /Zc:preprocessor standard preprocessor, /utf-8 source and execution character sets.
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:preprocessor /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    endif()
endfunction()

# ---- Dependencies (FetchContent) ---------------------------------------
include(FetchContent)

# nlohmann (engine libraries and app)
FetchContent_Declare(
    nlohmann                                                      # prefix for CMake variables
    GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
    GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA
    SYSTEM                                                        # Treat its headers as external (no /W4 /WX)
)
FetchContent_MakeAvailable(nlohmann)

# SQLite amalgamation (store). The archive has no CMakeLists.txt, so the library target is defined here.
FetchContent_Declare(
    sqlite
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip   # SQLite 3.50.4
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)

if(MORPHEUS_BUILD_LLAMA)
    # GPU backend for llama.cpp. FORCE so that toggling MORPHEUS_VULKAN reconfigures the fetched llama.cpp.
    set(GGML_VULKAN ${MORPHEUS_VULKAN} CACHE BOOL "" FORCE)

    # llama
    FetchContent_Declare(
        llama                                                         # prefix for CMake variables
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo TODO: redirect to the new repo: ggml-org/llama.cpp , probably update the targeted commit?
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

# ---- App (prototype, replaced in milestone 4) --------------------------
if(MORPHEUS_BUILD_APP)
    add_executable(morpheus
        src/main.cpp
    )

    # For morpheus cpp and h
    target_include_directories(morpheus
        PRIVATE # No sharing with other targets
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Warning flags
    if(MSVC)
        target_compile_options(morpheus PRIVATE /W4 /permissive- /Zc:preprocessor) # /W4 is Warning level, /permissive- is for standard compliance, /Zc:preprocessor is for MSVC preprocessor compliance
    else()
        target_compile_options(morpheus PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow) # -Wall is for the common warnings set, -Wextra is more warnings, -Wpedantic is for even more warnings, -Wconversion for warnings on narrowing casts, -Wshadow for shadowing
    endif()

    # C++20 only for morpheus (build fails on llama with C++20)
    target_compile_features(morpheus PRIVATE cxx_std_20)

    # Link morpheus
    target_link_libraries(morpheus PRIVATE
        llama
        SDL3::SDL3
        nlohmann_json::nlohmann_json
        imgui_sdl3
    )

    # ---- Defines ----
    target_compile_definitions(morpheus PRIVATE
        MORPHEUS_MODELS_DIR="${CMAKE_SOURCE_DIR}/third_party/models"
    )
endif()

# ---- Tests --------------------------------------------------------------
if(MORPHEUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
