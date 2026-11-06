# Task 1 report: Build foundation, test harness, project docs

Status: DONE

## Implemented
- CMakeLists.txt rewritten per brief (MORPHEUS_BUILD_APP / MORPHEUS_BUILD_TESTS / MORPHEUS_ASAN options, `morpheus_target_defaults`, llama/SDL3/imgui + `morpheus` target behind MORPHEUS_BUILD_APP, Catch2 fetch).
- CMakePresets.json (dev, tests, asan configure/build/test presets).
- src/core: Errors.h, ContentHash.h/.cpp (FNV-1a 64), CMakeLists.txt (`morpheus_core`).
- tests: CMakeLists.txt (glob, Catch2 discovery, ASan DLL copy), TestPaths.h, core/ContentHashTests.cpp.
- CLAUDE.md, README.md rewrite, `saves/` added to .gitignore after `game_projects/*/saved/`.

## TDD evidence
RED: `cmake --preset tests` (after test + CMake files, before ContentHash sources). Dependencies fetched, then:
```
CMake Error at src/core/CMakeLists.txt:1 (add_library):
  Cannot find source file:
    ContentHash.cpp
CMake Error: CMake can not determine linker language for target: morpheus_core
CMake Generate step failed.
```
GREEN: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
```
1/2 Test #1: ContentHash is streaming: chunks hash like their concatenation ... Passed
2/2 Test #2: ContentHash matches FNV-1a 64 reference values ................... Passed
100% tests passed out of 2
```
Focused: `./build/tests/bin/Debug/morpheus_tests.exe "[hash]"` -> All tests passed (4 assertions in 2 test cases).
Note: this CMake version prints "100% tests passed out of 2" (no "0 tests failed" text); same meaning.

## Files changed (commit 5f5eceb)
CMakeLists.txt, CMakePresets.json, CLAUDE.md, README.md, .gitignore, src/core/{CMakeLists.txt,Errors.h,ContentHash.h,ContentHash.cpp}, tests/{CMakeLists.txt,TestPaths.h,core/ContentHashTests.cpp}. 12 files.

## Deviations
- Commit trailer: used `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` (per the session's attribution instruction) instead of the brief's `Claude Opus 5.5`. Otherwise the commit command is as in the brief.
- No code deviations; all file contents transcribed verbatim.

## Self-review
- All files match the brief. Nothing outside the brief added. `dotnet/` not staged (ignored/untracked).
- Build clean under /W4 /WX for morpheus_core and morpheus_tests, no warnings.
- Not verified: `asan` and `dev` presets (not requested for this task; `dev` would fetch llama.cpp/SDL3/ImGui).

## Concerns
- Git warns LF will become CRLF on these files (autocrlf); harmless but files are checked in as LF.
- The ASan preset path (DLL glob in tests/CMakeLists.txt) was not exercised.
