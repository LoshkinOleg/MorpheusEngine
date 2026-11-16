add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
    HostedRecord.cpp
    JsonInt.cpp
    MemoryMode.cpp
    Settings.cpp
    TurnRecord.cpp
    StateSchema.cpp
    Changes.cpp
    Dice.cpp
    Text.cpp
    Thinking.cpp
    World.cpp
)
add_library(Morpheus::core ALIAS morpheus_core) # The name host projects link.
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
