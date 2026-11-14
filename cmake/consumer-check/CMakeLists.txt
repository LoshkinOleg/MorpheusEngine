# Consumer check: includes Morpheus the way a host project would (FetchContent), but from a local checkout, and checks
# that the vendored build defines only the engine libraries and morpheus_httplib. See the README, "Using Morpheus from another project".
cmake_minimum_required(VERSION 3.25)

# A C++-only host: Morpheus enables C itself for the SQLite amalgamation.
project(MorpheusConsumerCheck LANGUAGES CXX)

# The checkout to include: by default the repository this folder is in, two levels up.
cmake_path(SET MORPHEUS_DEFAULT_SOURCE_DIR NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MORPHEUS_SOURCE_DIR "${MORPHEUS_DEFAULT_SOURCE_DIR}" CACHE PATH "The MorpheusEngine checkout to include")

include(FetchContent)
# SYSTEM, as the README documents: Morpheus's headers count as system headers in the host's own files.
FetchContent_Declare(morpheus SOURCE_DIR "${MORPHEUS_SOURCE_DIR}" SYSTEM)
FetchContent_MakeAvailable(morpheus)

# With Morpheus's vendored defaults nothing beyond the six engine libraries and morpheus_httplib (cpp-httplib, fetched
# while MORPHEUS_BUILD_LLAMA_SERVER is on, as it is by default) exists: no app, tests or tools, no
# app-model or app-ui library, and none of llama.cpp, SDL3, ImGui or Catch2.
set(MORPHEUS_UNWANTED_TARGETS
    morpheus morpheus_tests morpheus_llama imgui_core SDL3::SDL3 Catch2::Catch2
    morpheus_app_model morpheus_app_ui morpheus_probe morpheus_play llama
)
foreach(unwanted IN LISTS MORPHEUS_UNWANTED_TARGETS)
    if(TARGET ${unwanted})
        message(FATAL_ERROR "The vendored Morpheus build defines the target '${unwanted}'; with its default options it must build only the engine libraries.")
    endif()
endforeach()
if(NOT TARGET Morpheus::turn)
    message(FATAL_ERROR "The vendored Morpheus build does not define Morpheus::turn.")
endif()
if(NOT TARGET morpheus_httplib)
    message(FATAL_ERROR "The vendored Morpheus build does not define morpheus_httplib (MORPHEUS_BUILD_LLAMA_SERVER defaults to ON).")
endif()
if(NOT TARGET Morpheus::grammar)
    message(FATAL_ERROR "The vendored Morpheus build does not define Morpheus::grammar.")
endif()

# The host's own program, with ordinary warnings and no warnings as errors.
add_executable(consumer_check main.cpp)
target_link_libraries(consumer_check PRIVATE Morpheus::turn)
if(MSVC)
    target_compile_options(consumer_check PRIVATE /W4)
else()
    target_compile_options(consumer_check PRIVATE -Wall -Wextra)
endif()
