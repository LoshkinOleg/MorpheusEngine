# Task 7 report: context rendering (state view, mechanics line, transcript)

Status: DONE
Commit: a7d8573 Render state view, mechanics lines and transcript (branch cpp)

## What was implemented

Namespace `morpheus::context`, library `morpheus_context`:
- `RenderStateView(const nlohmann::json& state)` in `src/context/StateView.{h,cpp}`: one line per entity, name order, numbers bare, strings quoted, list items bare, counters as `{item count}`. Throws `ContractError` via `MORPHEUS_REQUIRE` when an entity lacks `type` or `fields`.
- `RenderMechanicsLine(const core::TurnRecord&)` in `src/context/Transcript.{h,cpp}`: checks first, then applied changes; `set`, `add` (numeric / list item), `remove` (list item / counter item), `create`; engine note in parentheses; rejected changes omitted; empty string when there is nothing to report.
- `RenderTranscript(const std::vector<core::TurnRecord>&)` in `src/context/Transcript.cpp`: each turn becomes a User message (input, plus `\n` and the mechanics line when non-empty) and an Assistant message (narration).

Tests: `tests/context/RenderTests.cpp`, three `[context]` cases, written first and taken verbatim from the brief.

## TDD evidence

RED: `cmake --build --preset tests` after adding only the test file. Failed with:
```
C:\Users\user\Desktop\MorpheusEngine\tests\context\RenderTests.cpp(5,1): error C1083: Cannot open include file: 'context/StateView.h': No such file or directory [...morpheus_tests.vcxproj]
```

GREEN:
- `cmake --preset tests` (re-configure), then `cmake --build --preset tests`: builds `morpheus_context.lib` and `morpheus_tests.exe` with no warnings under `/W4 /WX`.
- `./build/tests/bin/Debug/morpheus_tests.exe "[context]"`: `All tests passed (10 assertions in 3 test cases)`.
- `ctest --preset tests`: `100% tests passed out of 91`, total 2.03 sec.

## Files changed

- `CMakeLists.txt`: added `add_subdirectory(src/context)` after `add_subdirectory(src/inference)`, outside the `if(MORPHEUS_BUILD_LLAMA)` block.
- `tests/CMakeLists.txt`: added `morpheus_context` to the `morpheus_tests` main `target_link_libraries` line.
- `src/context/CMakeLists.txt` (new): `morpheus_context` static library, links `morpheus_core` and `morpheus_inference`.
- `src/context/StateView.h`, `src/context/StateView.cpp` (new).
- `src/context/Transcript.h`, `src/context/Transcript.cpp` (new).
- `tests/context/RenderTests.cpp` (new).

Commit staged with the brief's `git add CMakeLists.txt src/context tests`. Nothing under `dotnet/`, `models/` or `settings.json` was staged.

## Deviations from the brief

None. Code is verbatim from the brief. The `[llama]` model variables were not needed: the `tests` preset has no llama.cpp.

## Concerns

- Git printed `LF will be replaced by CRLF the next time Git touches it` for the new and edited files. This is a line-ending configuration warning on Windows and did not affect the build or tests.
- Source and comments are ASCII only (checked with `LC_ALL=C grep`).
