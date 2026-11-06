# Task 4 report

Status: DONE. Commit 8c85c93 "Add the inference interface and build llama.cpp with Vulkan".

## Implemented
- src/inference (Llm.h, Llm.cpp, CMakeLists.txt): morpheus_inference, code verbatim from the brief.
- tests/fakes/FakeBackend.h and tests/inference/LlmTests.cpp, verbatim from the brief.
- Root CMakeLists.txt: options MORPHEUS_BUILD_LLAMA / MORPHEUS_VULKAN, FATAL_ERROR guard, llama block moved out of
  MORPHEUS_BUILD_APP into `if(MORPHEUS_BUILD_LLAMA)` with FORCE'd GGML_VULKAN and SYSTEM added (existing comments and TODO kept),
  add_subdirectory(src/inference).
- tests/CMakeLists.txt: /tests/llama/ filter when llama is off; morpheus_inference linked.
- CMakePresets.json: dev gets BUILD_LLAMA=ON and VULKAN=ON; tests gets BUILD_LLAMA=OFF (asan inherits).

## TDD evidence
- RED: `cmake --build --preset tests` -> `LlmTests.cpp(4,1): error C1083: Cannot open include file: 'fakes/FakeBackend.h'`.
- GREEN: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests` -> 100% tests passed out of 88;
  configure did not fetch llama.cpp (no llama-* in build/tests/_deps).
- asan preset: 100% tests passed out of 88.
- dev: `cmake --preset dev` found Vulkan 1.4.350 and glslc ("Including Vulkan backend", GGML_VULKAN:BOOL=ON in cache);
  `cmake --build --preset dev` exit 0. build/dev/bin/Debug contains ggml-vulkan.dll and morpheus.exe.
  `ctest --preset dev`: 100% passed out of 88.

## Deviations
- None to the design. Adding SYSTEM to the llama FetchContent block (as the brief shows) is a new behavior for the
  previously non-SYSTEM block; the dev build still succeeded.

## Concerns
- The dev build log has 35 "warning" lines (from fetched dependencies; none from MorpheusEngine targets, which build with /WX).
- Git prints LF->CRLF warnings on commit (autocrlf); files were written with LF.
