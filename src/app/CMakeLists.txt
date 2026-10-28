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
