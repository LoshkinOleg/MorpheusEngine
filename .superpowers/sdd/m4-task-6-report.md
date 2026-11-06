# M4 Task 6 report: Inspector helpers

Status: DONE

## Implemented
`app::Inspect` in morpheus_app_model: ChangeMark/MarkFor, PromptSection/PromptSections, RawPrompt, CallStats/CallStatsOf,
RetrievalRow/RetrievalRows, CheckText. Header, implementation and tests taken verbatim from the brief (extracted
mechanically from its code blocks). `model/Inspect.cpp` added to src/app/CMakeLists.txt.

## TDD evidence
- RED: `cmake --build --preset tests` with the test file and CMake entry in place but no header:
  `tests\app\InspectTests.cpp(10,1): error C1083: Cannot open include file: 'app/model/Inspect.h'`
  (With the header but the .cpp not yet generated into the project, the link failed with LNK2019 unresolved
  externals for all six functions.)
- GREEN: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[inspect]"`:
  `All tests passed (35 assertions in 5 test cases)`
- `ctest --preset tests`: `100% tests passed out of 223`

## Files changed
src/app/CMakeLists.txt, src/app/model/Inspect.h, src/app/model/Inspect.cpp, tests/app/InspectTests.cpp

## Self-review / concerns
- No warnings under /W4 /WX. No deviations from the brief.
- Note: after a failed CMake generate (source missing), the build did not regenerate until CMakeLists was touched.
