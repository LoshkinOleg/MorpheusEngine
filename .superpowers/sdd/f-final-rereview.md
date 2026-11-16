# Sub-project F: re-review of the final fix round

Range 6319acc..861622e (9906664, 861622e; 7 files, +55/-14), against `f-final-review.md` and the controller's
`f-final-fix-brief.md`. Read-only; no build run (the report records `[hosted]` 131 cases, `[prompts]` 18 cases and
ctest 671/671 passing).

## Findings, item by item

| Finding | Status | Evidence |
|---|---|---|
| I1 tail overflow blames the observation | Fixed | `HOSTED_TAIL_NAME` is now "the observation, the events, the player text and the resolve instruction (prompts/resolve)" (HostedSession.cpp:28-29). New test: a 4400-character resolve text gives a TurnError containing "(prompts/resolve) use" and "over tail_allowance", with no turn committed. The two existing tail tests are updated. |
| M1 speak overflow does not name the pack | Fixed | The DataError now starts "prompts/speak: the speak instruction for '<name>' uses" (HostedSession.cpp:263). The existing test is updated, and a new pack-based test checks "over narrate_allowance 128" and that no turn is committed. |
| Spec gap (no decision for the unchecked texts) | Fixed | Spec section 3 now says how resolve, speak, turn_cue, summarize and condense each overflow. Only section 3 was edited. It matches the code. |
| I2 live runs and asan | Controller's | The coordinator reports the live in-process `[llama][hosted]` cases, prompt_pack included, pass on Qwen3-4B at de579fa. Asan and the live `[server]` run are still to come. Not a code item. |
| M2 stale "L0 is engine text" comment | Fixed | PromptBuilder.cpp:224 now calls L0 the run's system text and says why the REQUIRE can only fire through a bug. The owned-path comment at :163 is rightly left alone. |
| M3 Open comment | Fixed | Re-wrapped. InferenceError now covers the system-text count, which happens before the file is touched. |
| M5 long line in PromptPack.cpp | Fixed | Wrapped. |
| Unused `EngineText.h` include | Fixed | Removed from HostedSession.cpp. No `EngineText.h` name is used there; `HOSTED_WORLD_ID` comes from HostedSession.h. |
| M4, M6 | Left (can wait) | M4 is now documented in spec section 3; M6 is cosmetic. |

The order slip the report mentions (sources edited before a RED run) is acceptable. Each new test pins a message the
old code could not produce ("prompts/speak: ...", "(prompts/resolve) use"), so neither test is vacuous.

## New observations (Minor, can wait)

- N1. `HOSTED_TAIL_NAME` is also the tail name of `HostedSession::Upkeep`'s `MemoryUpkeep`, which serves the catch-up
  and `Maintain`. There the tail holds `MAINTENANCE_INSTRUCTION`, not the resolve instruction, so a tail overflow found
  during upkeep would also mention prompts/resolve. It still names the real contents (observation, events) first, and
  that path runs only in the background, so this is harmless. If it matters later, pass a separate name from the upkeep
  caller.
- N2. `tests/hosted/HostedTurnTests.cpp:324` and `:347` are about 140 columns now. The PromptBuilder.cpp:224 comment
  wraps early (ragged at about 95 columns). Cosmetic.
- N3. Spec section 3 now covers `speak` twice (its own bullet, then "the per-turn DataError above" in the list), and the
  turn_cue bullet repeats "counts". Wording only.

## Assessment

**Ready to merge?** Yes

**Reasoning:** Every finding the controller decided is fixed as the brief specified, with tests that pin the new
messages, and the spec records the decision. The only open item is the controller's own verification gate (asan and
the live `[server]` run), which is not a code issue in this range.
