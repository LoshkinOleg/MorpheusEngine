add_library(morpheus_context STATIC
    StateView.cpp
    Transcript.cpp
)
target_include_directories(morpheus_context PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_context PUBLIC morpheus_core morpheus_inference)
morpheus_target_defaults(morpheus_context)
