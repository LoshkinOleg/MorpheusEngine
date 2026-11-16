# G1 final fix round: report

Status: DONE

Commits
- cfb4696 Decide: events recorded between turns count as input
- 3357443 Server: the done event carries timings.decide when a decide call ran
- 003fd3d Decide docs: the review's stale lines, the fixture list, two re-wrapped comments

## 1. Recorded events count as input
- HostedSession.cpp: `noInput = !playerText && newEvents.empty()`.
- New test "Events recorded between turns count as input for decide". RED before the fix: speaker was Saadia and 0 decide calls (expected Hulda, 1 call). GREEN after: `[decide]` 20 passed, 1 skipped, 236 assertions.
- Decide spec (owner-decisions bullet, section 2 "No input" bullet), README Decide paragraph and CHANGELOG 1.4.0 say "no new events, recorded since the last turn or sent with it".

## 2. done event timings.decide
- Api.cpp builds the done timings with resolve, speak and, when the record has it, decide.
- New test "The done event carries timings.decide only when a decide call ran" (decided turn has it, a following named-speaker turn does not). RED: decide missing; GREEN after the fix.
- README Decide paragraph says the done event's and the stored turn's timings carry decide.

## 3. Docs and comments
Server spec out-of-scope, hosted-world spec (line 19 and the "Later" bullet near line 246, same stale claim), prompt-pack spec (key list, "seven texts"), HostedPromptPackTests.cpp comment, decide spec Tests section (no Belethor; radiant variant's first-turn decide call), fixtures README (decide.json bullet, turn bullet re-wrapped), PromptBuilder.h comment re-wrapped.

## Final run
- `cmake --build --preset tests`: clean. `morpheus_tests.exe "[decide]"`: 20 passed, 1 skipped (236 assertions). `ctest --preset tests`: 100% tests passed out of 693 (real-model/server tests skipped as usual).
- ASCII and control-byte greps: 0 / 0 on all 13 changed files.

## Deviations
- Also fixed the hosted-world spec's "Later" bullet (~line 246) and the CHANGELOG/README wording, beyond the brief's literal lines, because they carried the same stale claims. CHANGELOG 1.4.0's timings line was not extended for the done event.
- Files are LF in the working tree (git warns about CRLF conversion); unchanged behavior.

## Review minors left
None addressed beyond the items above; the remaining minors in g1-final-review.md were left alone.
