# Every *.cpp under tests/ is a test source. CONFIGURE_DEPENDS re-globs on each build, so new files are picked up.
file(GLOB_RECURSE MORPHEUS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(morpheus_tests ${MORPHEUS_TEST_SOURCES})
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store Catch2::Catch2WithMain)
target_include_directories(morpheus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(morpheus_tests PRIVATE
    MORPHEUS_REPO_DIR="${PROJECT_SOURCE_DIR}"
    MORPHEUS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
morpheus_target_defaults(morpheus_tests)

# The ASan runtime DLL ships next to the MSVC compiler. Copy it beside the test executable so the tests also
# run outside a Visual Studio developer prompt.
if(MORPHEUS_ASAN)
    get_filename_component(MORPHEUS_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB MORPHEUS_ASAN_DLLS "${MORPHEUS_MSVC_BIN_DIR}/clang_rt.asan*dynamic-x86_64.dll")
    if(NOT MORPHEUS_ASAN_DLLS)
        message(FATAL_ERROR "No clang_rt.asan*dynamic-x86_64.dll found in ${MORPHEUS_MSVC_BIN_DIR}.")
    endif()
    add_custom_command(TARGET morpheus_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MORPHEUS_ASAN_DLLS} $<TARGET_FILE_DIR:morpheus_tests>
        VERBATIM)
endif()

# Register every Catch2 test case with CTest. PRE_TEST discovery runs at ctest time, after the ASan DLL copy.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(morpheus_tests DISCOVERY_MODE PRE_TEST)
