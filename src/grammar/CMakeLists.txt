add_library(morpheus_grammar STATIC
    Check.cpp
    Gbnf.cpp
    Schema.cpp
)
add_library(Morpheus::grammar ALIAS morpheus_grammar) # The name host projects link.
target_include_directories(morpheus_grammar PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_grammar PUBLIC morpheus_core nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_grammar)
