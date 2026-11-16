# G1 final review: the engine chooses the speaker (37a9176..3f0a80c)

Reviewed against `docs/superpowers/specs/2026-11-16-decide-speaker-design.md` (governing), the plan
`docs/superpowers/plans/2026-11-16-decide-speaker.md`, R8 of the Mantella brief and `CLAUDE.md`. Read-only: no build or
test was run (the controller reports ctest 691/691 at Task 3).

## Strengths

- The decide step slots into `HostedSession::RunTurn` without touching anything after it: every later use of
  `request.speakerId` became the chosen `speakerId` (catch-up, retrieval witness, the fit loop's `Entries`/`Pass`,
  `Upkeep`, the record, listeners), so a decided turn is exactly an explicit turn of that NPC, as the spec's goal says.
- Fail-fast ordering is right: every candidate passes the speaker checks (`checkSpeaker`) before the decide call, so
  nothing after the call can fail on the request's account; the no-candidate DataError and the decide-prompt TurnError
  both happen before any record (and the DataError before any call). Tests check "no call, no record" for both.
- The neutral view is honest and cheap: `Entries(..., witness "")` renders entries identically to a speaker's view
  (the witness only filters), so when the chosen speaker witnessed every turn, the decide prompt and the resolve prompt
  share the system message and all of L3: on llama-server the decide call warms the slot's KV cache for the resolve
  call instead of evicting it. The test that the decide system message equals the speaker's resolve system message
  pins this.
- Rollback is handled for free: `played` is `HostedTurns` (visible only) and `LatestHostedTurn` is visible-only, so the
  "last speaker" and "least recent" rules read the visible conversation; the decide step writes nothing, so the open
  rollback is abandoned only at commit, as before.
- LLM mistakes are not exceptions: parse failure and schema failure become `timings.decide.refused` with a
  deterministic fallback (least recent, never-spoke first, ties in join order). Tests cover both reason kinds and both
  tie rules.
- The Task 2 fix round brought the decide fit loop in line with the resolve tail (oldest new events leave first), and
  the two TurnError messages name what failed.
- The seed choice does not touch the owned game: `SamplingSeed` packs attempt below the kind bits (`attempt < 2^24`),
  so `CallKind::Resolve` with `1 << 22` cannot collide with the hosted resolve (attempt 0) or any realistic owned-game
  resolve retry, and `Replay.cpp` / `ModelSeat.cpp` (which switch on `CallKind`) see no new value.
- Reserved id: checked inside `Observe` and collected with its other faults, so every route that takes an observation
  (start, participants, turn) refuses it, while `ActorsOf` (stored observations) still reads old data.
- Server change is minimal and correct: `onSpeaker` updates a local the `action` lambda reads; `done` already used
  `record->speakerId`. API version, CHANGELOG, README, server spec and prompt-pack spec table all moved together; the
  fixture plays in-process, over HTTP (fake llama-server) and on the real model (validity only), as the spec asks.
- Test coverage of the spec's Tests list is essentially complete (candidates, one-candidate no-call, prompt shape and
  grammar, pack text and its variable check, refusals, onSpeaker order, cancel, reserved id, defaults route, fixtures,
  radiant-with-decide).

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

1. **`src/server/Api.cpp:432-435` - the `done` event drops `timings.decide`, while the docs promise it.**
   `done.timings` is built from `resolve` and `speak` only, so a host never sees the decide call or its refusal
   reasons in the stream; they reach it only through `GET /v1/runs/{id}/conversations/{cid}` (which dumps the stored
   `timings`). `CHANGELOG.md` ("A decided turn's `timings` gain `decide`") and the README Decide paragraph ("a decided
   turn's `timings` gain `decide`") read naturally as the `done` event's timings, which is the only timings object the
   README documents for a turn. It matters because the decide call adds a prefill and a short generation before the
   resolve call, i.e. directly to the host's time-to-first-sentence budget (the 1.5 s target), and the refusal reasons
   are the only log of a model that keeps failing the decide grammar. Fix (additive, keeps 1.4.0): when
   `record->timings.contains("decide")`, add it to `done.timings`; assert it in `ApiDecideTests` (and that a named
   turn's `done` has none). If the owner prefers the stream unchanged, then fix the two doc sentences to say "the stored
   turn's `timings` (GET conversation)" instead.

2. **`src/turn/HostedSession.cpp:304` - "no input" ignores events recorded through `/events`; the docs do not say so
   (spec question for the owner).** `noInput = !playerText && request.events.empty()` follows spec section 2 ("its
   request has no events"), but the owner decision in the same spec says "no player text and no events", and R8 says
   events recorded without a turn (`/events`) "are included in the next turn's volatile layer". `newEvents` (recorded
   since the last turn plus the request's) is what the tail shows under `# New events`; a host that records "A bard
   starts to sing" through `/events` and then asks `decide` with no text gets the last speaker excluded, while the same
   event sent in the request body lets anyone answer. That asymmetry is invisible to a host: README and CHANGELOG both
   say "no player text and no events". Fix: ask the owner. Either use `newEvents.empty()` (one-word change, plus a test
   with a recorded event; matches the owner decision and R8), or keep the request-only rule and say "no events in the
   request (events recorded through `/events` do not count)" in README, CHANGELOG and the `RunTurn` comment.

### Minor (Nice to Have)

1. Stale docs that now contradict the shipped behaviour (cheap; fix before merge):
   - `docs/superpowers/specs/2026-11-16-morpheus-server-design.md:190` Out of scope still lists R8's `"decide"`.
   - `docs/superpowers/specs/2026-11-16-hosted-world-design.md:19` "R8's `"decide"` waits for a later sub-project"
     (not in progress.md's list).
   - `docs/superpowers/specs/2026-11-16-prompt-pack-design.md:33` the unknown-key message lists six keys (the code
     and `PromptPackTests` now list `decide`); `:57` "the six texts".
   - `tests/hosted/HostedPromptPackTests.cpp:30` "A host's pack replacing all six texts": no longer all; either add a
     `decide` text (the pack tests then also cover it) or reword.
   - `docs/superpowers/specs/2026-11-16-decide-speaker-design.md` Tests names Belethor; `decide.json` has two NPCs.
     Also its radiant-variant line says "no decide call" while the test (correctly) expects one, on the first turn
     (no previous speaker, two candidates). Update the spec to what was built.
   - `tests/hosted/fixtures/README.md`: the top list has no `decide.json` bullet; the `turn` bullet's line is ~160
     columns.
   - README's 422 bullet ("the observation and player text do not fit the tail") does not mention that a decide turn's
     decide prompt can also be a 422 (its TurnError).
2. `src/context/PromptBuilder.h:~112` the reflowed `AssembleHostedPrompt` comment left one 163-column line; reflow.
3. `src/turn/HostedSession.cpp:318` a decide turn fails (400) when any one candidate fails `checkSpeaker`, e.g. the only
   allowed action needs an actor parameter whose scope offers only that candidate. Plan-mandated and rare (scopes only
   exclude the speaker), but host-visible and undocumented; consider leaving such a candidate out (DataError only when
   none remains), or document it.
4. Prompt cache: when the chosen speaker did not witness some turn of the conversation, the decide prompt's L3 diverges
   from its resolve prompt's at that turn, and the resolve call re-prefills from there (the slot holds the decide
   prompt). Inherent to the owner's neutral-view decision; worth one sentence in the spec, and worth measuring.
5. Tests not written (none blocks): a no-input turn with three NPCs present (a decide call whose enum leaves out the
   last speaker; today the exclusion is only seen through the one-candidate path); an answer cut at `maxTokens`; the
   context-overflow TurnError; the stop test cannot tell the `decided.cancelled` return from the catch-up's stop return
   (both commit nothing, which is what it asserts); `ApiDecideTests` and `invalid_requests.json` have no decide 400s
   (no NPC present; reserved `decide` actor), which the HTTP layer maps from DataError like any other.
6. `src/turn/HostedSession.cpp:30,380` the decide seed borrows `CallKind::Resolve` with a sentinel attempt. Correct
   today; a `CallKind::Decide` would be clearer once G2 adds call tags (it then needs `ModelSeat.cpp`/`Replay.cpp`
   cases). Note it in G2's plan.
7. The live `[server][decide]` test prints nothing about time; printing each turn's first-sentence time (as the
   conversation live test does) would measure what decide costs against the 1.5 s target.

## Minor triage (progress.md's per-task minors)

| Minor | Verdict |
|---|---|
| T1: candidates comment column (PromptBuilder.h) | Can wait (cosmetic). |
| T1: two long test lines | Can wait. |
| T1: HostedPromptTests variable loop omits decide | Can wait: the default decide text has no variable, so the loop would check nothing; `PromptPackTests` covers decide's variable rule. |
| T1: Observe reserved-id test single-fault only | Can wait: collection with other faults is the existing mechanism. |
| T2: lines >120 cols at PromptBuilder.h:112 (163) | Fix before merge (a reflow slip in a doc comment, one edit). HostedSession.cpp:339 and HostedDecideTests.cpp:335: can wait (the file already has several 121-132 col lines). |
| T2: context-overflow TurnError untested | Can wait (message-only branch; the tail branch is tested). |
| T2: named speaker's ProposalSchema/Fill computed twice | Can wait (cheap; computing once would need the schema threaded out of `checkSpeaker` for one caller). |
| T2: no test of an answer cut at maxTokens | Can wait (it is a parse failure, which is tested). |
| T3: server spec Out-of-scope lists decide (~190) | Fix before merge (doc contradicts behaviour). |
| T3: prompt-pack spec ~33 error key list, ~57 "six texts" | Fix before merge. |
| T3: decide spec names Belethor | Fix before merge (spec to match the fixture; also the radiant-variant sentence, Minor 1). |
| T3: fixtures README lacks a decide.json bullet; ~160-col line | Fix before merge (bullet); line length can go with it. |
| T3: ApiDecideTests happy path only | Can wait; cheapest coverage is two entries in `invalid_requests.json` (runs in-process and over HTTP). |
| T3: PlayFixture fills decisions twice in decide mode | Can wait (harmless: radiant has no `decide` replies; a comment would do). |

## Recommendations

- Resolve Important 2 with the owner before fixing Important 1's docs, since both touch the same README/CHANGELOG
  sentences; then do one doc pass covering Minor 1 and the triage's "fix before merge" rows.
- Run the `[server][decide]` live test once on the 9B model and record the decide call's cost in the README next to
  the existing first-sentence measurements.
- In G2, give decide its own `CallKind` and call tag together.

## Assessment

**Ready to merge?** With fixes

**Reasoning:** The engine logic is correct, well-ordered and well-tested against the spec, and it integrates cleanly
with per-speaker memory, catch-up, rollback, the prompt cache and the owned game's seeds. Before merge, the `done`
event should carry (or the docs should stop promising) `timings.decide`, the owner should settle whether `/events`
count as input, and the stale spec lines should be updated.
