# F final fix round: requirements

The final whole-branch review of sub-project F (the host's prompt pack) is `.superpowers/sdd/f-final-review.md`; read it
for the detail. The controller decided the items below (2026-11-15). Make each numbered item its own commit, tests
first where there is behaviour, in this order. HEAD at the start: 6319acc.

## 1. Overflow errors name the pack's texts (I1)

A host's `resolve` text sits in the tail and counts against `tail_allowance`; its `speak` text must fit
`narrate_allowance`. Neither is checked at open (the names they hold are only known at a turn). When they overflow, the
error must point the host at the text.

- `src/turn/HostedSession.cpp`: `HOSTED_TAIL_NAME` becomes
  `"the observation, the events, the player text and the resolve instruction (prompts/resolve)"`, so the tail's
  TurnError reads "the observation, the events, the player text and the resolve instruction (prompts/resolve) use N
  tokens, over tail_allowance M".
- The speak check's DataError becomes
  `"prompts/speak: the speak instruction for '<speaker name>' uses N tokens, over narrate_allowance M"` (today it starts
  `speaker: `).
- Tests: update the existing expectations in `tests/hosted/HostedTurnTests.cpp` (about lines 349, 375, 469) to the new
  texts. Add to `tests/hosted/HostedPromptPackTests.cpp`: a pack whose `resolve` text alone is over the test settings'
  tail_allowance (1024 tokens as the fake counts them: about 4200 characters) makes `RunTurn` throw a TurnError whose
  message contains "(prompts/resolve) use"; a pack whose `speak` text is over narrate_allowance (128: about 600
  characters) makes `RunTurn` throw a DataError whose message starts "prompts/speak: the speak instruction for 'Hulda'
  uses". Neither turn commits (`session.Turns(id)` stays empty).
- Spec `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`, section 3: add the decision for the texts not checked
  at open: `resolve` (in the tail: a turn over tail_allowance is the TurnError above, naming `prompts/resolve`),
  `speak` (the per-turn DataError, now naming `prompts/speak`), `turn_cue` (in L3's entries: it counts in the
  transcript, which folds), `summarize` and `condense` (not budget-checked; an oversize text makes the fold or condense
  take the engine's existing fallback summary, as any over-long memory call does). Edit only section 3.

## 2. Comments the review lists

- `src/turn/HostedSession.cpp`: remove the `#include "turn/EngineText.h"` if nothing in the file uses it any more (check
  with a grep for every name EngineText.h defines; keep it if one is used).
- `src/turn/HostedSession.h`, `Open`'s comment: re-wrap the paragraph (no orphan short line), and make the InferenceError
  sentence cover the `CountTokens` call of the system-text check, made before the file is touched, as well as embedding.
- `src/context/PromptBuilder.cpp` (about line 224) and any comment there that calls the hosted L0 "engine text": say it
  is the run's system text (the host's pack or the engine's default). Leave the `MORPHEUS_REQUIRE` as it is (Open has
  already refused an over-cap system text, so it can only fail through an engine bug).
- `src/turn/PromptPack.cpp`: wrap the line over 120 columns.

## Finally

- `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"` and
  `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`, then `ctest --preset tests`: all pass.
- Foreground builds only, one at a time; leave nothing running.
- ASCII and control-byte greps on every changed file (both print 0).
- Report what you changed per item, the commands and their output, and anything from the review's minors you left.
