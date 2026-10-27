add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
    StateSchema.cpp
    Changes.cpp
    Dice.cpp
)
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
