# Task 7 report
Implemented core::SceneDecl, World::scene, loader validation (DataErrors naming world.json), sandcrawler scene declaration, per brief verbatim.
RED: `cmake --build --preset tests` -> C2039 'scene': is not a member of 'morpheus::core::World' (WorldTests.cpp, SandcrawlerWorldTests.cpp).
GREEN: `morpheus_tests.exe "[world]"` -> All tests passed (89 assertions in 8 test cases); `ctest --preset tests` -> 100% passed out of 276.
Files: src/core/World.h, World.cpp, game_projects/sandcrawler/world.json, tests/core/WorldTests.cpp, SandcrawlerWorldTests.cpp.
Deviations: none. Concerns: none.
