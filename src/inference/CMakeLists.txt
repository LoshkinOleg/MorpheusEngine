add_library(morpheus_inference STATIC
    Llm.cpp
)
add_library(Morpheus::inference ALIAS morpheus_inference) # The name host projects link.
target_include_directories(morpheus_inference PUBLIC ${PROJECT_SOURCE_DIR}/src)
# No library dependencies: Llm.cpp uses only core/Errors.h (ContractError), which is header-only.
morpheus_target_defaults(morpheus_inference)
