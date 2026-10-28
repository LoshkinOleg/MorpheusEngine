add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
    JsonInt.cpp
    Settings.cpp
    TurnRecord.cpp
    StateSchema.cpp
    Changes.cpp
    Dice.cpp
    Text.cpp
    World.cpp
)
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
