# G2 Task 2 report: the call tag

Commit: 22ab09c "Call tags: every turn-layer call names itself; the replay and the app's labels read the tag".

## Implemented
- `GenerationSpec::callTag` (last member). Set in Session (lookup, resolve, review, narrate/renarrate in the narrate lambda), HostedSession (decide, resolve, speak), MemoryUpkeep (summarize, condense).
- `turn::CallKindOf(spec)` and `app::SeatLabelOf(spec)` read the tag; other tags throw ContractError with the specified messages. `RecordingBackend::Complete` and `ModelSeatBackend` updated. Dropped now-unused includes.
- Comments in ResolveGrammar.h/.cpp and Gbnf.h no longer say the root line exists for CallKindOf.
- Tests: ReplayTests and ModelSeatTests cases replaced as briefed (old CallKindName checks moved in verbatim); new tests/turn/CallTagTests.cpp (3 cases, tag `[calltag]`) covers all seven tags: one assisted+check turn gives exactly lookup, resolve, narrate, review, renarrate, review; a fold case checks summarize; a condense case checks condense; each asserts every tag is owned. HostedDecideTests/HostedPromptPackTests got the briefed tag checks.

## TDD evidence
Deviation from process: the implementation was written before the tests were compiled, so no RED run was captured. GREEN: `cmake --build --preset tests --clean-first` (clean), `morpheus_tests.exe "[calltag],[replay],[model_seat]"`: All tests passed (3777 assertions in 27 test cases). `ctest --preset tests`: 100% passed out of 698 (only the model/server-gated tests skipped).

## Deviations
- Removed the replay test "A resolve grammar is not taken for a lookup when the world has a field named lookups": it tested grammar sniffing, which no longer exists.
- ModelSeatBackendTests, ReplayTests (RecordingBackend cases): bare specs now carry a callTag ("narrate", "lookup", "summarize", "resolve"), since the seat/recorder throw on an untagged call. ModelSeatBackendTests NARRATE_PROMPT comment reworded.
- ReplayTests, ModelSeatTests: added `catch_matchers_string.hpp` (needed for CHECK_THROWS_WITH with a string); named-away an unused `prompt` parameter (/W4 /WX). ModelSeatTests lost its now-unused includes.
- Source edits preserved each file's existing line endings (Session.cpp was mixed; normalized to CRLF).

## ASCII checks
Both greps printed 0 for every changed file (none of them contains an escape sequence).

## Concerns
None. No tool or src/llama code used CallKindOf/SeatLabelOf; the `dev` preset was not built.
