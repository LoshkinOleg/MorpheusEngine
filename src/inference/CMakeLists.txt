add_library(morpheus_inference STATIC
    Llm.cpp
)
add_library(Morpheus::inference ALIAS morpheus_inference) # The name host projects link.
target_include_directories(morpheus_inference PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_inference PUBLIC morpheus_core)
morpheus_target_defaults(morpheus_inference)
