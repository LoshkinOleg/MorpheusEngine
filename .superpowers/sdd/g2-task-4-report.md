# G2 Task 4 report: the backends use the thinking format

Status: DONE_WITH_CONCERNS. Commit db59f02 "Backends: the thinking block's texts from the settings' thinking format".

## Implemented
- inference/Llm.h: THINKING_* and CLOSE_THINKING_TEXT removed.
- LlamaConfig and LlamaServerConfig gain `core::ThinkingSetting thinking`; both backends resolve it with core::ResolveThinking (in-process: end must be one token, InferenceError otherwise; llama-server: empty skip is not tokenized, stop string from `endText`).
- app main, morpheus_server, probe, play pass `settings.thinking`; EngineHost::Start's sameBackend compares it.
- README (thinking_format, server recommendation, two wording updates) and architecture spec sentence.
- Tests: constants replaced by core::QWEN_THINKING; new backend, Complete and EngineHost cases; live tests (in-process and server) run Auto then Format/QWEN_THINKING (tests/llama/LlamaTestModels.h gained MakeThinkingBackend(setting), SharedThinkingBackend uses it).

## TDD evidence
Concern: I implemented the source change before the new tests compiled, so there is no RED run captured (the tests would have failed to compile on the missing `thinking` member). GREEN:
- `cmake --build --preset tests --clean-first`: clean, no warnings.
- `morpheus_tests.exe "[llamaserver]"`: All tests passed (292 assertions in 36 test cases).
- `"[engine_host]"`: All tests passed (56 assertions in 6 test cases).
- `ctest --preset tests`: 100% tests passed out of 704 (live tests skip).
- `cmake --build build/dev --config RelWithDebInfo`: exit 0, no warnings (ccache note only). Live tests were not run (no models/servers).

## Deviations
- LlamaServerBackend::ThinkingMarkers also gained `openText` (besides the brief's `endText`): the "template already ends with the open marker" cut in TokenizePrompt needs the format's open text, since the constant is gone.
- Live in-process test: second backend loaded for the Format run (MakeThinkingBackend); loads the thinking model twice in one test.
- EngineHost test is a new case beside the existing one.

## Byte checks
ASCII greps on every changed source/test file: both print 0. No backslash-escape sequences were meant in new source except "\n" literals in tests, checked to be literal.

## Files changed
src/inference/Llm.h, src/llama/LlamaCppBackend.{h,cpp}, src/llamaserver/LlamaServerBackend.{h,cpp}, src/app/main.cpp, src/app/model/EngineHost.cpp, tools/{server,probe,play}/main.cpp, README.md, architecture spec, tests/llamaserver/{LlamaServerBackendTests,LlamaServerCompleteTests,LlamaServerLiveTests}.cpp, tests/app/EngineHostTests.cpp, tests/llama/{LlamaTestModels.h,LlamaThinkingTests.cpp}.

## Fix round
Items 1-6 done (docs for "none", LlamaCppBackend ctor comment, llama-server open cut only when the template has the enable_thinking switch (`templateWritesOpen`) with a fake-server test using open "\n", re-wrapped comments, architecture spec bullets, `[llama]` test for a multi-token end marker plus the second-copy note).
- `cmake --build --preset tests`: exit 0, no errors/warnings.
- `morpheus_tests.exe "[llamaserver]"`: All tests passed (293 assertions in 37 test cases).
- `ctest --preset tests`: 100% tests passed out of 705.
- `cmake --build build/dev --config RelWithDebInfo`: exit 0, no errors/warnings. `[llama]` tests not run (no models).
- ASCII greps on changed source/test files: 0 and 0.
- Test-first note: the new tests were written with the fix in the same round, no separate RED run.
