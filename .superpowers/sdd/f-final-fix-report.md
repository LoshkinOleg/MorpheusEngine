# F final fix round: report

Commits: 9906664 (item 1), 861622e (item 2).

## Item 1
- HostedSession.cpp: HOSTED_TAIL_NAME and the speak DataError use the brief's texts ("prompts/speak: ...").
- HostedTurnTests.cpp: three expectations updated.
- HostedPromptPackTests.cpp: two new tests (resolve text over tail_allowance -> TurnError containing "(prompts/resolve) use"; speak text over narrate_allowance -> DataError "prompts/speak: the speak instruction for 'Hulda' uses"); neither turn commits.
- Spec section 3: decision for resolve, speak, turn_cue, summarize and condense added.

## Item 2
- EngineText.h include removed from HostedSession.cpp (no name from it is used there; it built). That one-line removal went into commit 1, since the file is shared.
- HostedSession.h Open comment re-wrapped; InferenceError covers the system-text CountTokens.
- PromptBuilder.cpp (hosted, about line 224): L0 called the run's system text. The non-hosted comment at line 163 still says "engine text", correctly (engine rules there).
- PromptPack.cpp over-120 line wrapped.

## Verification
- `cmake --build --preset tests` ok; `[hosted]`: all passed (1464 assertions, 131 cases); `[prompts]`: all passed (151 assertions, 18 cases); `ctest --preset tests`: 100% passed out of 671.
- ASCII and control-byte greps on all seven changed files: 0 and 0.

## Deviations
- I edited sources before running the RED test run (order slip), so there is no recorded RED output. The tests assert the new texts, which the old code did not produce.
- A first build I let run in the background collided on the PDB with a second; I stopped it and rebuilt in the foreground. Nothing is left running.

## Review minors left
None touched beyond the brief.
