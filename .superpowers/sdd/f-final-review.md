# Sub-project F (the host's prompt pack): final review

Range c0d9a52..6319acc (6 commits, 33 files, +854/-95). Reviewed against `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`
(governing), `docs/superpowers/plans/2026-11-16-prompt-pack.md`, R7 and CLAUDE.md. Read-only; no build run.

## Strengths

- Plan alignment is complete. Every component of spec section 4 is present with the planned signatures (`PromptVariables`,
  `PromptText`, `PROMPT_TEXTS`, `PromptPack::{FromHost, Fill, Text}`, `HostData::prompts`, `HostedWorld::Prompts()`,
  the open-time cap check, the defaults route, API 1.3.0). No unjustified deviation found.
- One source of truth. Every hosted text now flows through `_world.Prompts()`: a grep finds no remaining use of the
  `HOSTED_*` constants outside `EngineText.h` and `PromptPack.h`. `FillVariables` and the `{actor}` loop are gone, as the
  spec asked; `Session` (Morpheus's own game) still uses `SUMMARIZE_INSTRUCTION`/`CONDENSE_INSTRUCTION` untouched.
- Backward compatibility is handled properly: `HostDataToJson` omits `prompts` when empty, an empty `prompts` object
  is the same as none, and the test pins open.json's pre-F hash (`35856ce76ac0af2e`). `std::map` plus nlohmann's sorted
  objects make the hash independent of the host's key order, so `RunWorker::Reopen`'s same-hash early return behaves.
- Fail-fast at the right boundary: shape faults in `HostDataFromJson` (pointer-named), content faults in `FromHost`
  (key, variable and what the text takes), the system cap in `HostedSession::Open` before the file is touched. A failed
  reopen falls back to the previous host data through the existing `RunWorker::Reopen` path, unchanged.
- `Fill` is correct and stricter than what it replaces: single left-to-right pass (a name containing `{actor}` is not
  re-scanned, tested), literal braces kept, and an unfilled or unknown variable is a ContractError rather than silently
  emitting an empty name. Every caller's names are non-empty by construction (`core::ReadActors` requires non-empty
  names; `ActorName` falls back to the id).
- The prompt cache's stable prefix is preserved: L0 has no variables, the pack is fixed for the session's lifetime, and
  the R7 acceptance test checks byte equality of the stable messages over three turns under a custom pack.
- Tests cover every item of the spec's test list, including the condense call (a deliberately small summary_cap with a
  told-by copy), the reopen-with-changed-pack path (`worldHash` updated, next tail shows the new text), the in-process,
  HTTP and live-model runs of a new `prompt_pack.json` fixture, and the server route's exact variables JSON.
- Docs are consistent with the code: README (table row, open body, the Prompt pack paragraph, 1.3.0 in the stream
  example), CHANGELOG, the server spec's route table and doc list, the hosted-world spec's pointer, the fixtures README,
  CLAUDE.md's layout line.

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**I1. A host's long resolve instruction is reported as the observation being too long.**
`src/turn/HostedSession.cpp:28` (`HOSTED_TAIL_NAME`), used by `MemoryUpkeep::Fit` (`src/turn/MemoryUpkeep.cpp:55`).
The resolve instruction sits in the tail (`# Task\n<instruction>`) and counts toward `tail_allowance`. Before F it was a
fixed 20-token engine string; now it is host data with no budget check (spec section 3 covers only `system` and
`speak`). A host that puts its action guidance in `resolve` (plausible: it is the natural place for "prefer `none`
unless...") and overflows the tail gets a 422 on every turn saying "the observation, the events and the player text use
N tokens, over tail_allowance M", which points the host away from the real cause. This contradicts the spirit of the
spec's Errors section (pack faults name `prompts/<key>`). Cheapest fix: make the name say what the tail holds, e.g.
"the observation, the events, the player text and the resolve instruction (prompts/resolve)"; optionally also refuse at
open a `resolve` whose unfilled text alone (variables removed) exceeds `tail_allowance - summary_cap`. It is a spec gap,
so record the decision in the spec's section 3 either way.

**I2. The live runs of the new defaults are not yet recorded.**
Spec section 5: "The live `[llama][hosted]` and `[server]` tests run on the new defaults before F is done." progress.md
says Task 5's live llama case was unbuilt by the implementer and no live run is recorded for F. The two default-text
changes (`HOSTED_SPEAK_INSTRUCTION`, the system text's "Most turns need no action" sentence) change every existing
hosted run's L0 and speak message, so this is the only evidence that they help rather than hurt on Qwen3-4B. Not a code
fix: run `[llama][hosted]` (including the new prompt-pack case) and the `[server]` live tests, plus asan, and record
them in progress.md before merging.

### Minor (Nice to Have)

- M1. `src/turn/HostedSession.cpp:263-266`: the speak overflow is a per-turn DataError beginning "speaker: the speak
  instruction for 'Hulda'...". Under a host pack the fault is in `prompts/speak`; naming it ("prompts/speak: filled for
  'Hulda' uses N tokens, over narrate_allowance M") would match the rest of F. `HostedTurnTests.cpp:349` pins the
  current prefix and would change with it. An open-time lower-bound check (the text with its variables removed) would
  catch the always-failing case at open.
- M2. `src/context/PromptBuilder.cpp:224` (and `:163` in the owned path, which is fine): "L0 is engine text (a bug if too
  long)" is stale for the hosted path; L0 may now be the host's text, checked at open, which is why it is still a bug
  here. One-line comment update.
- M3. `src/turn/HostedSession.h:88-94`: the Open comment's InferenceError sentence ("cannot be reached or fails while
  embedding: either way a new file is then removed") no longer covers the new `CountTokens` call, which can throw
  InferenceError before any file exists; also the orphan line wrap after "an owned". (Same as the Task 3 minor.)
- M4. No length bound on `summarize`/`condense`. A pathological text makes every fold and condense miss the context and
  take `SummarizeFallback`/`CondenseFallback` silently. Pre-existing mechanism, harmless in practice; worth a sentence in
  the spec's section 3 ("not budget-checked; an oversize text degrades to the engine's fallback summary").
- M5. `src/turn/PromptPack.cpp:105`: ~128-column line. `Fill` also hardcodes speaker/player/actor rather than reading
  `PROMPT_TEXTS` (the spec says the table is shared by "the check, Fill and the defaults route"); acceptable, since
  `PromptVariables`' members are the real source of what Fill can supply, but note the drift from the spec wording.
- M6. `FromHost`'s unknown-key message prints the raw key (`prompts/a/b`), while `HostDataFromJson` escapes pointer
  tokens. Cosmetic.

## Minor triage (the per-task minors in progress.md)

| Task | Minor | Verdict |
|---|---|---|
| 1 | PromptPack.cpp one line ~128 cols | Can wait (fix if the file is touched for M5) |
| 1 | No test that a throwing FromHost leaves defaults | Drop: FromHost returns nothing when it throws, so there is no state to observe |
| 2 | HostedWorldTests two lines ~125 cols | Can wait |
| 2 | No test of an escaped prompts key pointer | Can wait (shape path shared with lore/clock, already escaped) |
| 2 | Clock-vs-pack fault order unpinned | Can wait; the spec does not define it |
| 3 | HostedSession.cpp:17 `EngineText.h` include unused | Fix before merge if touching the file for I1 (trivial); otherwise can wait |
| 3 | HostedSession.h Open comment wrap and InferenceError sentence | Fix with I1 (doc accuracy, see M3) |
| 3 | HostedPromptPackTests unused includes | Can wait |
| 5 | HTTP fixture case cannot show the pack was used | Can wait: ApiPromptTests proves the pack reaches the prompt through the Api, and the in-process fixture checks prompt_absent |
| 5 | Duplicated clock/prompts handling in the two fixture helpers | Can wait (brief-mandated; extract when a third field appears) |

## Recommendations

- Fold I1, M1, M2, M3 and the unused include into one small commit; they all touch the "a host text over a budget"
  story and the comments around it.
- Amend spec section 3 to state the decision for `resolve`, `turn_cue`, `summarize` and `condense` (no open-time
  check; how an overflow surfaces), so a later sub-project does not rediscover it.
- When G adds sampling/thinking fields as data, reuse `PROMPT_TEXTS`' pattern (a constexpr table shared by the check
  and the defaults route); it worked well here.

## Assessment

**Ready to merge?** With fixes

**Reasoning:** The implementation matches the spec and plan closely, keeps existing runs' hashes and Morpheus's own
game untouched, and is well tested; the one code issue (I1) is a cheap error-naming fix for a spec gap, and the spec's own
gate of live runs on the new defaults (I2) has not yet been recorded.
