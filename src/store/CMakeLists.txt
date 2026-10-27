add_library(morpheus_store STATIC
    Sqlite.cpp
    RunStore.cpp
)
target_include_directories(morpheus_store PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_store
    PUBLIC morpheus_core
    PRIVATE sqlite3
)
morpheus_target_defaults(morpheus_store)
