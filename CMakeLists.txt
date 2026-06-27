cmake_minimum_required(VERSION 3.24)  # 3.24+ for FetchContent OVERRIDE_FIND_PACKAGE

project(MorpheusEngine
    VERSION 1 # Major only notation: 1, 2, 3, 56, 103, etc.
    LANGUAGES CXX
)

# ---- Global settings ----------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF) # Enforce strict standard syntax, no compilier specific lingo.

# Put all binaries in one place — handy on Windows for DLL discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For executables: .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # For shared libs: .so on linux
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # For static libs: .a on linux, .lib on win

# ---- Options ------------------------------------------------------------
# Example: option(MORPHEUS_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# ---- Dependencies (FetchContent) ---------------------------------------
include(FetchContent)

# llama
FetchContent_Declare(
    llama                                                         # prefix for CMake variables
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git     # repo
    GIT_TAG        960d628f46d17ea9c10aa944b061b2341afeb3b5       # commit SHA to get, version b9804
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA 
)
FetchContent_MakeAvailable(llama)

# nlohmann
FetchContent_Declare(
    nlohmann                                                      # prefix for CMake variables
    GIT_REPOSITORY https://github.com/nlohmann/json.git           # repo
    GIT_TAG        55f93686c01528224f448c19128836e7df245f72       # commit SHA to get, 3.12.0
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA 
)
FetchContent_MakeAvailable(nlohmann)

# SDL3
FetchContent_Declare(
    sdl3                                                          # prefix for CMake variables
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git          # repo
    GIT_TAG        8e37db5e797b6167f3a00d697d816a684bd259c7       # commit SHA to get, SDL3 3.4.10
    GIT_SHALLOW    FALSE                                          # Has to be FALSE to be able to use a commit SHA 
)
FetchContent_MakeAvailable(sdl3)

# imgui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/LoshkinOleg/cmake-ready-imgui-sdl3.git
    GIT_TAG        5acd6de3ed4efb06d665628b01fba0fe3e63f7bc
    GIT_SHALLOW    FALSE # Has to be FALSE to be able to use a commit SHA 
)
FetchContent_MakeAvailable(imgui)

# ---- Main target --------------------------------------------------------
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
	imgui::imgui
)