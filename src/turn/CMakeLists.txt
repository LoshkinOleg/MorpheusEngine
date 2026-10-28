add_library(morpheus_turn STATIC
    Resolve.cpp
    ResolveGrammar.cpp
)
target_include_directories(morpheus_turn PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context)
morpheus_target_defaults(morpheus_turn)
