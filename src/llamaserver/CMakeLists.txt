add_library(morpheus_llamaserver STATIC
    LlamaServerBackend.cpp
)
add_library(Morpheus::llamaserver ALIAS morpheus_llamaserver) # The name host projects link.
target_include_directories(morpheus_llamaserver PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_llamaserver
    PUBLIC morpheus_inference morpheus_core
    PRIVATE nlohmann_json::nlohmann_json morpheus_httplib
)
morpheus_target_defaults(morpheus_llamaserver)
