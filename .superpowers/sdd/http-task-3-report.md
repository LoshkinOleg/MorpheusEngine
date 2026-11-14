# Task 3 report: CheckGrammar leaves the interface

Commit eb25c2a "Take CheckGrammar off LlmBackend: the model seat checks with grammar::Check".

Implemented exactly per the brief: LlmBackend is the bare union of Tokenizer, ChatModel, Embedder; ModelSeat::Check uses
grammar::Check (morpheus_app_model links morpheus_grammar); LlamaCppBackend::CheckGrammar is a plain public method;
RecordingBackend, ModelSeatBackend, FakeBackend, ScriptedBackend lost their overrides; ScriptedBackend lost
SetGrammarVerdict / GrammarChecks / GrammarCheck; tests updated (ReplayTests case deleted, UiTests no longer uses atomic).

## TDD
RED: `cmake --build --preset tests && morpheus_tests.exe "[model_seat]"`:
  CHECK( checked->problem == "the grammar rejects 'm' at the start" ) with expansion `"" == "the grammar rejects 'm' at the start"`.
GREEN: `cmake --build --preset tests` no warnings; `ctest --preset tests`: 100% tests passed out of 374.
Dev: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`:
  all four built, no warnings. `grep -rn CheckGrammar src tests tools` shows only LlamaCppBackend.h/.cpp,
  LlamaGrammarTests.cpp, plus comments in grammar/Check.cpp; no GrammarVerdict/GrammarChecks left.
[llama] grammar tests (models exported): `morpheus_tests.exe "LlamaCppBackend::CheckGrammar*,*grammar*[llama]"`:
  test cases 10, 9 passed, 1 skipped (LlamaTestModels.h:63, the thinking-model test: the model has no thinking mode),
  assertions 4938 all passed.

## Deviations
- The brief's filter `"llama.cpp*" "LlamaCppBackend::CheckGrammar*"` (two args) matched nothing in this Catch2 build; I used a
  single comma-joined filter instead. The brief's line numbers differed slightly from the files; edits were by text.
- Byte check: perl check_bytes printed nothing on all 15 files; `grep -n $'[\x80-\xFF]'` printed nothing; the control-byte
  grep -c could not run under this shell's locale (grep -P unsupported); the edits contain no \u or \x sequences
  (git diff grep for `\[ux]hex` printed nothing).

## Concerns
None. Working tree still has the pre-existing .superpowers/sdd/progress.md modification and reference submodule noise (not staged).
