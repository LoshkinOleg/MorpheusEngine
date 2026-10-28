# The app's state and actions, without SDL, ImGui or llama.cpp: every preset builds and tests it.
add_library(morpheus_app_model STATIC
    model/AppController.cpp
    model/Catalog.cpp
    model/EngineHost.cpp
    model/Inspect.cpp
    model/RunModel.cpp
    model/SettingsEdit.cpp
)
target_include_directories(morpheus_app_model PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn)
morpheus_target_defaults(morpheus_app_model)

# The ImGui windows over AppController. They need only ImGui's core, so the tests draw them headless.
add_library(morpheus_app_ui STATIC
    ui/Ui.cpp
    ui/RunsWindow.cpp
    ui/PlayWindow.cpp
    ui/StateWindow.cpp
    ui/TurnLogWindow.cpp
    ui/ContextWindow.cpp
    ui/MemoryWindow.cpp
    ui/SettingsWindow.cpp
)
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_ui PUBLIC morpheus_app_model imgui_core)
morpheus_target_defaults(morpheus_app_ui)

if(MORPHEUS_BUILD_APP)
    # The app: the SDL3 window, ImGui's SDL backends and the llama.cpp backend.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama imgui_sdl3)
    morpheus_target_defaults(morpheus)
    # F5 in Visual Studio runs it from the repository root, where settings.json, game_projects/ and saves/ are.
    set_target_properties(morpheus PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
