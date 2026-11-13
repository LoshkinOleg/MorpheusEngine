# Build Task 1 report

Status: DONE. Commit f662d69 "Choose llama.cpp's GPU backend, default the build options to the top-level project".

## Implemented
Steps 2-7 of the brief verbatim: top-level-only output dirs, options defaulting to PROJECT_IS_TOP_LEVEL, validated MORPHEUS_GPU_BACKEND, conditional /WX / -Werror, GGML_VULKAN/GGML_CUDA from the backend with FORCE, ggml-org URL (TODO removed), startup project at top level only, presets use MORPHEUS_GPU_BACKEND (dev Vulkan, dev-asan CPU).

## TDD evidence
RED (before): configure with Metal succeeded (exit 0, MORPHEUS_GPU_BACKEND unused warning); CPU config gave GGML_VULKAN=ON, MORPHEUS_GPU_BACKEND:UNINITIALIZED, TreatWarningAsError count 4; line 84 still had ggerganov + TODO.
GREEN (after): Metal -> "MORPHEUS_GPU_BACKEND is 'Metal'; it must be one of: Vulkan, CUDA, CPU." exit 1. CPU -> exit 0, GGML_CUDA OFF, GGML_VULKAN OFF, STRING=CPU, WARNINGS_AS_ERRORS BOOL=OFF, TreatWarningAsError 0, Level4 4. Back to Vulkan -> GGML_VULKAN ON, GGML_CUDA OFF. URL line 117 has no TODO. `grep MORPHEUS_VULKAN` in CMakeLists/presets/src/tools/tests: empty.
Step 9: tests preset builds with no warnings, ctest 100% passed out of 315, TreatWarningAsError 4, MORPHEUS_VULKAN in cache 0.
Step 10: dev configured (Generating done), GGML_CUDA OFF, GGML_VULKAN ON, GPU_BACKEND Vulkan, MORPHEUS_VULKAN 0; morpheus built (exit 0, no warnings outside llama.cpp's _deps, which are llama's own); `morpheus.exe --frames 120` exit 0; asan preset configures.

## Files changed
CMakeLists.txt, CMakePresets.json (only these staged).

## Deviations
None. Edits applied with a Python script (exact replacements, line endings preserved).

## Concerns
None. Warnings seen in the dev build come from llama.cpp/ggml sources and Windows SDK headers, not Morpheus targets.
