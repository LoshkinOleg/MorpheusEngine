add_library(morpheus_llama STATIC
    LlamaCppBackend.cpp
)
target_include_directories(morpheus_llama PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llama
    PUBLIC morpheus_inference
    PRIVATE llama
)
morpheus_target_defaults(morpheus_llama)
