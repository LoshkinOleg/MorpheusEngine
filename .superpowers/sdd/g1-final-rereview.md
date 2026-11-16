# G1 final fix round: re-review (3f0a80c..003fd3d)

Read-only: I read the diff `.superpowers/sdd/review-3f0a80c..003fd3d.diff`, the brief `g1-final-fix-brief.md` and the
report `g1-final-fix-report.md`, and spot-checked the working tree. I ran no build or test. The report gives
ctest 693/693 and `[decide]` 20 passed, 1 skipped.

## The earlier findings

### I1: the `done` event carries `timings.decide`. Resolved.
- `src/server/Api.cpp`: the done timings are resolve and speak. `decide` is added only when the record has it. The change is additive, so API 1.4.0 still holds.
- `tests/server/ApiDecideTests.cpp`: the new test checks both sides. The decided turn's `done` event has
  `timings.decide`, and a named-speaker turn after it in the same conversation has none.
- README's Decide paragraph now names both places (the `done` event and the stored turn). CHANGELOG's "A decided turn's
  `timings` gain `decide`" is now true for both, so the ambiguity is gone.

### I2: events recorded between turns count as input. Resolved, as the owner decided.
- `src/turn/HostedSession.cpp`: `noInput = !playerText && newEvents.empty()`. At that point `newEvents` already holds
  the events recorded since the last turn plus the request's events.
- New test "Events recorded between turns count as input for decide": the report says it failed before the fix and
  passes after it. After a named Hulda turn and one recorded event, a decide turn with no text makes a decide call
  that offers both candidates, and Hulda (the last speaker) speaks again. The existing tests still fit the new rule:
  - radiant.json has no events steps, so the radiant-with-decide variant still makes exactly one call.
  - decide.json's events turn sends its event with the request.
  - The live test's `noInput` check reads only the request body, which is still correct because decide.json records
    no events between turns.
- The new wording ("no new events, recorded since the last turn or sent with it") is in the spec's owner-decisions
  bullet and section 2, in README and in CHANGELOG.

### The doc minors. Resolved.
- Server spec, out of scope: done.
- Hosted-world spec, line 19 and the "Later" bullet: done. The second fix went beyond the brief, and was right to.
- Prompt-pack spec, key list and "seven texts": done.
- HostedPromptPackTests comment: done.
- Decide spec, Tests section (no Belethor; the radiant variant's first-turn call): done.
- Fixtures README (the decide.json bullet, the re-wrapped `turn` bullet): done.
- PromptBuilder.h re-wrap: done.

## New or leftover minors (none blocks)

1. `src/turn/HostedSession.h:141`: the `RunTurn` comment still says "no player text and no events". Say "no new events
   (recorded since the last turn or sent with the request)" to match the code and the spec.
2. `docs/superpowers/specs/2026-11-16-morpheus-server-design.md:56`: the `done` event's `timings` "holds the resolve
   and speak times". Add "and `decide` when a decide call ran (1.4.0)".
3. `tests/hosted/HostedPromptPackTests.cpp:30`: the reworded comment is 136 columns. Re-wrap it.
4. `CHANGELOG.md` 1.4.0: the reflow left a short line ("is left out while another remains. With one candidate").
   Cosmetic.
5. The original review's other minors (Minor 3-7 and the "can wait" rows of its triage) stay as they were.

## Assessment

**Ready to merge?** Yes

**Reasoning:** The fixes resolve both Important findings and all the doc items the brief listed, with tests written
first. None of the changes adds a Critical or Important issue. What is left is two stale comment lines and a line
length, which can go with the next docs touch.
