add_library(morpheus_store STATIC
    Sqlite.cpp
    RunStore.cpp
)
add_library(Morpheus::store ALIAS morpheus_store) # The name host projects link.
target_include_directories(morpheus_store PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_store
    PUBLIC morpheus_core
    PRIVATE morpheus_sqlite3
)
morpheus_target_defaults(morpheus_store)
