# E1 Task 5 report: every NPC catches up; R6's three-actor fixture

Status: DONE_WITH_CONCERNS (all tests pass; concerns below are about observables, not failures)

Commit: 422037e "Every NPC catches up on what it witnessed: before it speaks and in the background; R6's three-actor fixture"

## What I implemented

- `HostedSession::CatchUp(conversationId, actorId, throughTurn, seedTurn, stop)` (private), as in the brief: early
  return when the actor's mark already reaches `throughTurn`; otherwise `Upkeep(..., observation, seedTurn, false)
  .FoldAll(...)` over `Entries(now, RememberedThrough, throughTurn, actorId)` re-read each pass, then
  `SetRememberedThrough(throughTurn)` when the mark is still short. The observation (the conversation's last turn's)
  is passed to the new `Upkeep(..., observation, ...)` signature for the tellers' names.
- `HostedSession::NpcWitnesses(turns)` (private), verbatim from the brief: speakers and non-player listeners, first-seen
  order.
- `RunTurn`: catches the speaker up to `conversation.foldedThrough` right after the request's checks (after the speak
  instruction's narrate_allowance check and the new-events list), before retrieval; nullopt when cancelled.
- `Maintain`: replaced Task 4's interim "last speaker" folding with the brief's loop: an ended conversation with
  unfolded turns gets `MarkFolded` + `SyncHistory`, then every NPC witness of every conversation catches up through
  the conversation's cut.
- Header comments for `CatchUp`, `NpcWitnesses`, `Maintain` and `RunTurn` updated; `MAINTENANCE_INSTRUCTION`'s comment
  now names the catch-up.
- `tests/hosted/fixtures/three_actors.json` (verbatim) and a "Memory fixtures (`three_actors.json`)" section in
  `tests/hosted/fixtures/README.md` (plus a line in the Files list).
- `tests/hosted/HostedTestSupport.h`: `test::BELETHOR`, `test::MemoryEchoResponder` (uses `IsHostedFold` /
  `IsHostedCondense` instead of literal prefixes, as instructed), `test::PlayMemoryFixture` (verbatim runner), and the
  includes it needs.
- Tests: the fixture test in HostedFixtureTests.cpp and the catch-up test in HostedMemoryTests.cpp.

## TDD evidence

RED (`cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[memory]"`, before the
implementation):

```
HostedTestSupport.h(247): FAILED:  "" contains: "finest in Whiterun"   (three_actors.json step 10: Belethor)
HostedTestSupport.h(247): FAILED:  "" contains: "takes bribes"         (step 12: Hulda)
HostedTestSupport.h(247): FAILED:  "" contains: "Did I miss anything"  (step 12: Hulda)
HostedMemoryTests.cpp(233): FAILED: REQUIRE_FALSE( saadia.empty() )   with expansion: !true
test cases:  17 |  15 passed | 2 failed
assertions: 137 | 133 passed | 4 failed
```

(As predicted: only the ended conversation's last speaker, Saadia, had a memory; the catch-up test's
`REQUIRE(folded >= 1)` passed and it failed on Saadia having none.)

GREEN (after the implementation and the test updates below):

```
[memory]: All tests passed (139 assertions in 17 test cases)
[hosted]: All tests passed (972 assertions in 87 test cases)
[turn]: All tests passed (163 assertions in 16 test cases)
[replay]: All tests passed (501 assertions in 7 test cases)
[context]: All tests passed (236 assertions in 29 test cases)
[hosted-server]: All tests passed (1071 assertions in 54 test cases)
ctest --preset tests: 100% tests passed out of 592 (4 skipped: llama-server / real-server tests)
```

([replay]'s assertion count varies run to run (505, 497, 501) independent of this change.) The build showed no
warnings.

Escape/byte checks on every changed file (`LC_ALL=C grep -c $'[\x80-\xFF]'` and the control-byte grep): all 0 0
(HostedSession.h/.cpp, HostedTestSupport.h, HostedFixtureTests.cpp, HostedMemoryTests.cpp, HostedFoldingTests.cpp,
fixtures/README.md, three_actors.json, ApiTurnTests.cpp, HttpServerTests.cpp, RunWorkerTests.cpp). The `"\n- "`,
`'\n'` and `"\n"` literals in HostedTestSupport.h are still literal backslash sequences.

No morpheus or ctest process left running.

## Files changed

- src/turn/HostedSession.h, src/turn/HostedSession.cpp
- tests/hosted/fixtures/three_actors.json (new), tests/hosted/fixtures/README.md
- tests/hosted/HostedTestSupport.h, tests/hosted/HostedFixtureTests.cpp, tests/hosted/HostedMemoryTests.cpp,
  tests/hosted/HostedFoldingTests.cpp
- tests/server/ApiTurnTests.cpp, tests/server/HttpServerTests.cpp, tests/server/RunWorkerTests.cpp

## Deviations from the brief, and why

1. Catch-up test sizes (the brief said to tune them). With the echo responder a memory holds the folded lines
   verbatim, so the brief's 3000-byte lines at a 4096 context cannot work: two such lines (about 1500 tokens) do not
   overflow L3 (about 2270 tokens), and lines long enough to overflow it would echo into a memory over summary_cap 512,
   which a condense (also an echo) cannot shrink (DataError). Used a 2500 context (L3 about 680 tokens) and
   1600-byte lines (400 tokens): one fits, two fold, and the echoed memory (about 410 tokens) fits summary_cap 512.
   The comment in the test explains the numbers.
2. `CatchUp` reads the conversation through `MORPHEUS_REQUIRE(now.has_value(), ...)` instead of dereferencing
   `*_store.Conversation(...)` directly, as the rest of the file does.
3. D1's Maintain tests in HostedFoldingTests.cpp, updated for the new order (MarkFolded and SyncHistory before any
   NPC's fold):
   - "A cancelled Maintain folds nothing..." renamed "A cancelled Maintain writes no memory and can simply run again";
     after the cancelled Maintain the conversation's foldedThrough is now 2 (its turns left the transcript) while
     Hulda's memory is still empty.
   - "A stop between two ended conversations...": `StopAfterEmbed::Arm()` became `Arm(int embeds)` (stop after that
     many embeds); armed with 2 (c1's turn as history, then Hulda's memory), so c2 is marked folded (2) but Saadia's
     fold is cancelled; the next Maintain writes only Saadia's memory (one call).
   - Comments that said the summary is "the last speaker's" now say "the only NPC witness's".
4. Server tests (outside the brief's commit paths, but "[hosted-server]" must pass and these depended on the old
   order). `folded_through` no longer signals that a memory landed, since MarkFolded moves it before the fold call:
   - ApiTurnTests "A turn during a background fold stops it...": the last check waited for folded_through == 1 (now
     true at once) and then needed foldCalls >= 2 (failed). It now waits for foldCalls >= 2, then a GET (which waits for
     the running fold) checks folded_through == 1.
   - RunWorkerTests (both fold tests): they now check `session.Memories(test::HULDA).size() == 1` instead of
     foldedThrough (they passed unchanged, but foldedThrough no longer proves the fold landed; the second one was
     racy).
   - HttpServerTests: only the comment changed (the first history embed now happens as the upkeep starts; the GET
     waits for the fold, and the check of 5 completions proves the fold call).
   I committed these three server test files with the task.

## Concerns

- In Maintain an ended conversation's `MarkFolded` + `SyncHistory` run before any stop check, so a cancelled Maintain
  leaves the conversation folded (its turns retrievable by their witnesses) while its NPCs' memories wait for the next
  Maintain. This is the brief's code as written; the tests now document it. A host reading `folded_through` through the
  API can no longer tell from it that the memories are written. Task 7's memories route will be the right observable,
  and ApiTurnTests' check could then move to it.
- `NpcWitnesses` has one caller for now (Maintain); the brief says Task 6's ShareMemories will be the second.
