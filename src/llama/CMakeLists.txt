add_library(morpheus_llama STATIC
    LlamaCppBackend.cpp
)
add_library(Morpheus::llama ALIAS morpheus_llama) # The name host projects link.
target_include_directories(morpheus_llama PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference morpheus_core
    PRIVATE llama
)
morpheus_target_defaults(morpheus_llama)
