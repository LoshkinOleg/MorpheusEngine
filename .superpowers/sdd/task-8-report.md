# Task 8 report: ScriptedBackend and the memory index

## Implemented
- tests/fakes/ScriptedBackend.h (brief verbatim; added <algorithm>, <span>, <stop_token>, <utility>; sqrt computed once before the normalize loop).
- tests/fakes/FakeBackend.h: dropped `final`, comment updated.
- src/turn/Retrieval.h/.cpp (brief verbatim), Retrieval.cpp added to src/turn/CMakeLists.txt.
- tests/turn/RetrievalTests.cpp (brief verbatim plus catch_matchers.hpp and catch_matchers_exception.hpp includes for MessageMatches).

## TDD evidence
- RED: `cmake --build --preset tests` -> `RetrievalTests.cpp(16,1): error C1083: Cannot open include file: 'turn/Retrieval.h'` (implementation files and CMake entry temporarily withheld).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[retrieval]"` -> All tests passed (23 assertions in 5 test cases).
- `ctest --preset tests`: 147/147 passed. `ctest --preset asan` (rebuilt): 147/147 passed.

## Commit
See git log (message: "Add the memory index for lore, history and note retrieval").

## Deviations
- Only the include additions above (mechanical). No ranking flake occurred; no assertion loosened.

## Concerns
- None. Note ScriptedBackend::_calls / _embeddedTexts are unsynchronized; tests using it across threads must only read after the worker finishes.
