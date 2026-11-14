# morpheus_server's library (morpheus_server spec, section 4): run workers, the transport-free API and the HTTP
# adapter. It is not one of the engine libraries a vendored build exposes, so it has no Morpheus:: alias.
add_library(morpheus_server_lib STATIC
    RunWorker.cpp
    SentenceSplitter.cpp
    SlotBackend.cpp
    SlotPool.cpp
)
target_include_directories(morpheus_server_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_server_lib
    PUBLIC morpheus_turn morpheus_llamaserver nlohmann_json::nlohmann_json
    PRIVATE morpheus_httplib
)
morpheus_target_defaults(morpheus_server_lib)
