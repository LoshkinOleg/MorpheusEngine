add_library(morpheus_turn STATIC
    Folding.cpp
    HostedSession.cpp
    HostedWorld.cpp
    MemoryUpkeep.cpp
    PromptPack.cpp
    Replay.cpp
    Resolve.cpp
    ResolveGrammar.cpp
    Retrieval.cpp
    Review.cpp
    Session.cpp
    Worker.cpp
)
add_library(Morpheus::turn ALIAS morpheus_turn) # The name host projects link.
target_include_directories(morpheus_turn PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_grammar)
morpheus_target_defaults(morpheus_turn)
