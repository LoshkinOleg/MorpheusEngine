# Task 1 report: LlmBackend::CheckGrammar

Status: DONE. Implemented exactly as in the brief (interface, LlamaCppBackend, RecordingBackend forward, FakeBackend throw, ScriptedBackend verdict/records, both tests). No deviations.

## TDD
RED: `cmake --build --preset tests` after adding the tests only:
- ReplayTests.cpp(392): error C2039: 'CheckGrammar': is not a member of 'morpheus::turn::RecordingBackend'
- ReplayTests.cpp(393): error C2039: 'SetGrammarVerdict': is not a member of 'morpheus::test::ScriptedBackend'
- (also GrammarChecks, 3 more lines)

GREEN:
- `cmake --build --preset tests` clean (no warnings); `morpheus_tests.exe "[replay]"`: All tests passed (511 assertions in 8 test cases).
- `ctest --preset tests`: 100% tests passed out of 286.
- `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests morpheus morpheus_play morpheus_probe`: built, no warnings.
- With MORPHEUS_TEST_CHAT_MODEL / MORPHEUS_TEST_EMBED_MODEL set: `build/dev/bin/RelWithDebInfo/morpheus_tests.exe "LlamaCppBackend::CheckGrammar*"`: All tests passed (8 assertions in 1 test case). (llama.cpp logs the expected "error parsing grammar" for the unparsable-grammar case.)

## Files
src/inference/Llm.h, src/llama/LlamaCppBackend.h/.cpp, src/turn/Replay.h/.cpp, tests/fakes/FakeBackend.h, tests/fakes/ScriptedBackend.h, tests/turn/ReplayTests.cpp, tests/llama/LlamaGrammarTests.cpp.

## Concerns
None.
