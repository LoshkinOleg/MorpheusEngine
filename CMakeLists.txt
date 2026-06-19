cmake_minimum_required(VERSION 3.24)  # 3.24+ for FetchContent OVERRIDE_FIND_PACKAGE

project(MorpheusEngine
    VERSION v1
    LANGUAGES CXX
)

# ---- Global settings ----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # TODO: what's this?

# Export compile_commands.json for clangd / IDE tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # TODO: explain?

# TODO: question: safe to remove?
# Default to Release if nothing was specified (single-config generators)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Put all binaries in one place — handy on Windows for DLL discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # TODO: this is configurable and emits binaries inside the project right? Not some kind of global folder full of binaries?
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---- Options ------------------------------------------------------------
# Example: option(MORPHEUS_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# ---- Dependencies (FetchContent) ---------------------------------------
include(FetchContent)

# TODO: explain
# Example pinning pattern — fill in as you go:
#
# FetchContent_Declare(
#     llama_cpp
#     GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
#     GIT_TAG        <pinned-commit-sha>
#     GIT_SHALLOW    TRUE
# )
# FetchContent_MakeAvailable(llama_cpp)

# ---- Main target --------------------------------------------------------
add_executable(morpheus
    src/main.cpp
)

target_include_directories(morpheus
    PRIVATE # TODO: remind me
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Warning flags
if(MSVC)
    target_compile_options(morpheus PRIVATE /W4 /permissive- /Zc:preprocessor) # TODO: ?
    if(MORPHEUS_WARNINGS_AS_ERRORS)
        target_compile_options(morpheus PRIVATE /WX) # Set up MSVC to treat warnings as errors.
    endif()
else()
    target_compile_options(morpheus PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow) # TODO: ?
    if(MORPHEUS_WARNINGS_AS_ERRORS)
        target_compile_options(morpheus PRIVATE -Werror)
    endif()
endif()

# target_link_libraries(morpheus PRIVATE
#     llama
#     # SDL2::SDL2
#     # nlohmann_json::nlohmann_json
#     # ...
# )