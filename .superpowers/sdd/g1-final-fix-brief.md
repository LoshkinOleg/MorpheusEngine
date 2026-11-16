# G1 final fix round: requirements

The final whole-branch review of sub-project G1 (the engine chooses the speaker) is `.superpowers/sdd/g1-final-review.md`;
read it for the detail. The controller and the owner decided the items below (2026-11-16). Make each numbered item its
own commit, tests first where there is behaviour, in this order. HEAD at the start: 3f0a80c.

## 1. Events recorded between turns count as input (I2, the owner's decision)

"No input" means: no player text after cleaning, and no new events at all since the conversation's last turn: neither
events recorded through `RecordEvents` (POST .../events) since that turn, nor events sent with the request. Both kinds
are the turn's new events (`newEvents` in `RunTurn`), and both show in the tail.

- `src/turn/HostedSession.cpp`, `RunTurn`: `noInput` becomes `!playerText && newEvents.empty()`.
- Test in `tests/hosted/HostedDecideTests.cpp`: Hulda, Saadia and Aela; a named turn by Hulda; then `RecordEvents` with
  one event; then a "decide" turn with no player text and no request events makes a decide call (both candidates; the
  script's decision names Hulda, the last speaker, and she speaks). The existing tests stay green.
- Spec `docs/superpowers/specs/2026-11-16-decide-speaker-design.md`: the owner-decisions bullet (line 12) and section 2's
  "No input" bullet (line 23) say "no new events (recorded since the conversation's last turn or sent with the
  request)". README's Decide paragraph and the CHANGELOG 1.4.0 entry say the same ("no events" becomes "no new events,
  recorded since the last turn or sent with it").

## 2. The `done` event carries `timings.decide` (I1)

- `src/server/Api.cpp`, the `done` event's `timings`: add `"decide"` (the record's `timings.at("decide")`) when the
  record's timings contain it. Nothing else changes (still API 1.4.0: an addition).
- Test in `tests/server/ApiDecideTests.cpp`: the decided turn's `done` event has `timings.decide`; a turn with a named
  speaker (a second turn in the same test, or a new one) has none.
- README's Decide paragraph: say that the `done` event's and the stored turn's `timings` carry `decide`.

## 3. Docs and comments the review lists

- `docs/superpowers/specs/2026-11-16-morpheus-server-design.md` (about line 190, "Out of scope"): R8's `"decide"` is
  done (G1, 1.4.0); keep the optional review and the call tag as later sub-projects (G3, G2).
- `docs/superpowers/specs/2026-11-16-hosted-world-design.md` (about line 19): the line that says `"decide"` is not
  supported or is later: say G1 added it (`2026-11-16-decide-speaker-design.md`).
- `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`: line ~33's unknown-key error text lists the seven keys
  ("system, resolve, speak, turn_cue, decide, summarize, condense"), and line ~57's "six texts" says seven (`decide`
  added by G1).
- `tests/hosted/HostedPromptPackTests.cpp` (about line 30): the comment that says the pack replaces "all six texts":
  make it true for the pack it builds (it does not send `decide`; say "the six texts it names" or similar).
- The decide spec's Tests section: `decide.json` has Hulda and Saadia with Aela (no Belethor); the radiant variant
  makes one decide call, on its first turn (no turn before it), then alternates with no call.
- `tests/hosted/fixtures/README.md`: a `decide.json` bullet in the fixture list at the top, like the others; re-wrap the
  ~160-column `turn` bullet to about 120 columns.
- `src/context/PromptBuilder.h` (about line 112): re-wrap the 163-column comment line to about 120 columns.

## Finally

- `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "[decide]"`, then `ctest --preset tests`:
  all pass. Foreground builds only, one at a time, never in the background. If a test you did not touch crashes (SIGSEGV
  or a "Debug Assertion Failed" dialog), do one `cmake --build --preset tests --clean-first` before changing code.
- ASCII and control-byte greps on every changed file (both print 0).
- Report per item what changed, the commands and their output, and which review minors you left.
