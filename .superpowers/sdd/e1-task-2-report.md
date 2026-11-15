# E1 Task 2 report: SummaryScope

Status: DONE

Commit: 46599c2 "SummaryScope: the memory upkeep reads and writes summaries through a scope; the owned game passes the run-wide one"

## What was implemented

- `turn::SummaryScope` (src/turn/MemoryUpkeep.h), verbatim from the brief. `MemoryUpkeep::StoreFold` is gone; the
  constructor takes `SummaryScope scope` and requires `shown`, `storeFold`, `storeCondensed`, `condenseMessage` and a
  non-empty `foldInstruction` ("MemoryUpkeep: a scope needs its summaries, its stores and its instructions").
  `FoldOldest` measures and calls with `_scope.foldInstruction` and stores via `_scope.storeFold`; `Condense` reads
  `_scope.shown()`, builds `_scope.condenseMessage(summaries)`, measures `CondensePrompt(systemMessage, message)`,
  calls `CondenseSummaries(systemMessage, summaries, message, ...)` and stores via `_scope.storeCondensed`. `_pStore`
  stays for the seed.
- Folding: `SummarizePrompt(systemMessage, entries, instruction)`, `SummarizeTurns(systemMessage, entries, instruction,
  spec, backend, stop)`, `CondensePrompt(systemMessage, message)`, `CondenseSummaries(systemMessage, summaries, message,
  spec, backend, stop)`. Folding.cpp no longer includes EngineText.h.
- `Session::Upkeep` passes the run-wide scope exactly as the brief gives it.
- `HostedSession::Upkeep` passes the speaker's scope: `shown` = `ActorSummaries(speakerId, false)`, `storeFold` =
  Task 1's fold (actor and conversation set, `FoldConversation`, `SyncHistory()`), `storeCondensed` sets `actorId` and
  calls `CondenseActorSummaries(speakerId, ...)`; engine texts (SUMMARIZE_INSTRUCTION / CONDENSE_INSTRUCTION).
- `context::SummaryPlace { System, Tail }` in PromptBuilder.h; `LayOut` takes `place` after `budget`, with the brief's
  `summaryExcess` / `tailOverBudget` / `systemShare` code; `systemShare` replaces `systemCost - summaryExcess` in the
  transcript budget and its DataError. Both assemblers pass `SummaryPlace::System`.
- Tests: the brief's MemoryUpkeep scope test and `Harness::Upkeep` run-wide scope; a hosted test (below).

## Hosted condense test (closes Task 1's hosted-condense ContractError)

tests/hosted/HostedFoldingTests.cpp, "A speaker's memories over summary_cap condense into one, superseding the old ones
and leaving others' alone" [hosted][folding]: summary_cap 12 (one memory is 9 FakeBackend tokens, two are 14). Saadia
folds c1; Hulda folds c2, then c3; the c3 Maintain folds and then condenses Hulda's two memories in one condense call
(last call ends with CONDENSE_INSTRUCTION). Hulda shows one condensed memory 2-3, actor Hulda, no conversation; the two
it condensed (c2, c3) stay superseded; Saadia's c1 memory is untouched.

## TDD evidence

RED 1 (hosted test, before any src change; built, then run):
```
./build/tests/bin/Debug/morpheus_tests.exe "A speaker's memories over summary_cap condense into one\, superseding ..."
  RunStore::Summaries: this is a hosted run [D:\MorpheusEngine\src\store\RunStore.cpp:1225]
  FAILED: CHECK( backend.Calls().back()...content == std::string(turn::CONDENSE_INSTRUCTION) )  (last call was the fold)
  FAILED: REQUIRE( shown.size() == 1 )  with expansion: 2 == 1
test cases: 1 | 1 failed
```
RED 2 (upkeep test added): `cmake --build --preset tests`
```
MemoryUpkeepTests.cpp(92,15): error C2039: 'SummaryScope': is not a member of 'morpheus::turn'
```
GREEN: `cmake --build --preset tests` (no warnings, no errors), then:
```
[upkeep]     All tests passed (58 assertions in 7 test cases)
[turn]       All tests passed (163 assertions in 16 test cases)
[replay]     All tests passed (493 assertions in 7 test cases)
[context]    All tests passed (224 assertions in 27 test cases)
[hosted]     All tests passed (897 assertions in 77 test cases)
[folding]    All tests passed (182 assertions in 15 test cases)
[model_seat] All tests passed (3249 assertions in 18 test cases)
ctest --preset tests: 100% tests passed out of 581 (4 [server]/llama-server tests skipped as usual)
```
No Session test and no replay fixture was edited.

## Files changed

src/turn/Folding.h, src/turn/Folding.cpp, src/turn/MemoryUpkeep.h, src/turn/MemoryUpkeep.cpp,
src/context/PromptBuilder.h, src/context/PromptBuilder.cpp, src/turn/Session.cpp, src/turn/HostedSession.cpp,
tests/turn/MemoryUpkeepTests.cpp, tests/hosted/HostedFoldingTests.cpp, tests/turn/FoldingTests.cpp,
tests/app/ModelSeatTests.cpp

## Deviations

- Commit includes three test files the brief's commit step does not list: tests/turn/FoldingTests.cpp and
  tests/app/ModelSeatTests.cpp (callers of the Folding functions; Step 3 says update every caller; they now pass
  turn::SUMMARIZE_INSTRUCTION / turn::CONDENSE_INSTRUCTION, assertions unchanged), and tests/hosted/HostedFoldingTests.cpp
  (the hosted condense test the dispatch asked for; put there because the plan's Task 4 creates HostedMemoryTests.cpp).
- Harness comment in MemoryUpkeepTests.cpp updated ("Its StoreFold" -> its run-wide scope's fold); `<utility>` added.
- Source comments do not name "Task 4": HostedSession says the scope uses "the engine's fold and condense texts", and
  AssembleHostedPrompt says the speaker's memories "sit in the system message (L2)".
- One comment in HostedSession::Maintain changed from "Upkeep's StoreFold" to "the scope's storeFold".

## Byte checks

`LC_ALL=C grep -c $'[\x80-\xFF]'` and `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'` print 0 and 0 for all 12
changed files. No escapes were written. No morpheus or ctest process left running.

## Concerns

- tests/app/ModelSeatTests.cpp keeps a comment saying a summary call ending with neither instruction "cannot be built";
  with the instruction now a parameter it can be built, though nothing in the engine does yet. Left as is (app scope,
  owned texts only); worth revisiting when Task 4 gives hosted runs their own texts, since ModelSeat/Replay/CallKindOf
  recognise memory calls by the exact engine instructions.
