# The app's state and actions, without SDL, ImGui or llama.cpp: every preset builds and tests it.
add_library(morpheus_app_model STATIC
    model/AppController.cpp
    model/Catalog.cpp
    model/EngineHost.cpp
    model/Inspect.cpp
    model/ModelSeat.cpp
    model/ModelSeatBackend.cpp
    model/RunModel.cpp
    model/SettingsEdit.cpp
)
add_library(Morpheus::app_model ALIAS morpheus_app_model) # The name host projects link.
target_include_directories(morpheus_app_model PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_model PUBLIC morpheus_turn morpheus_grammar)
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
    ui/ModelSeatWindow.cpp
)
add_library(Morpheus::app_ui ALIAS morpheus_app_ui) # The name host projects link.
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_ui PUBLIC morpheus_app_model imgui_core)
morpheus_target_defaults(morpheus_app_ui)

if(MORPHEUS_BUILD_APP)
    # The app: the SDL3 window, ImGui's SDL backends, and the llama.cpp and llama-server backends.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama morpheus_llamaserver imgui_sdl3)
    if(WIN32)
        # shell32 and ole32: the Windows fonts folder, for the UI font.
        target_link_libraries(morpheus PRIVATE shell32 ole32)
    endif()
    morpheus_target_defaults(morpheus)
    # F5 in Visual Studio runs it from the repository root, where settings.json, game_projects/ and saves/ are.
    set_target_properties(morpheus PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
