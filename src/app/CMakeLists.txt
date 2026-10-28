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
)
target_include_directories(morpheus_app_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_app_ui PUBLIC morpheus_app_model imgui_core)
morpheus_target_defaults(morpheus_app_ui)
