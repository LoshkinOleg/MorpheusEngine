# D1 final review - fix report

Commit: 88c9adb "D1 final review fixes: ..." (15 files; nothing under .superpowers/, docs/references/, settings.json, imgui.ini or models/ staged).

Base 283b108 (499 tests in `tests`, 3 `[server]` skipped). All nine items fixed; behaviour of Morpheus's own game is
unchanged (no existing `Session`/turn/replay test edited; item 5 adds one test to `tests/turn/SessionTests.cpp` and
one include, `store/RunStore.h`, that it needs).

## Items

1. **D1 contract (documentation).**
   - `src/turn/HostedSession.h:47-54` class comment: one `HostedSession` per run file, two on one file not supported;
     not thread-safe, the caller serializes every call on a run (turns of different conversations included);
     `Maintain` runs between them and stops through its stop token; every model call uses the default slot
     (`GenerationSpec::slot` left at -1); a slot per conversation and concurrency across conversations are D2's,
     addable without changing this API.
   - Hosted spec `docs/superpowers/specs/2026-11-16-hosted-world-design.md:47-54` ("D1's contract" list in section 1,
     HostedSession) and the D2 out-of-scope bullet (:241). Architecture spec hosted section, HostedSession bullet
     (`docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:178`). D2's design is not pre-decided.
2. **Event overflow wedges a conversation (documented, behaviour unchanged).**
   - `HostedSession.h:79-82` (`RecordEvents`) and `:98-104` (`RunTurn`): every event recorded since the last turn
     joins the next tail; when they alone exceed `tail_allowance`, every later `RunTurn` of that conversation throws
     `TurnError` (before the resolve call) until it ends; `EndConversation` still works.
   - Hosted spec section 6 (:203), `RecordEvents` bullet in section 1 (:61); architecture spec HostedSession bullet.
   - Test `tests/hosted/HostedTurnTests.cpp:377` "Events recorded between turns over the tail allowance make every
     later turn a TurnError until the conversation ends": two 2400-byte recorded events; three `RunTurn`s (with player
     text, without, again) each throw `TurnError` with no model call and no committed turn; `EndConversation` and
     `Maintain` then work (folded through 1). It pins current behaviour, so it passed on first run (no RED possible).
3. **Interleaved conversations (tests).**
   - `tests/hosted/HostedFoldingTests.cpp:208` "Two open conversations interleave their turns; ending one folds only
     its turns, which the other retrieves": c1 (Hulda, Aela) and c2 (Saadia, Aela) alternate turns 1-4 with an event
     recorded in each before each turn. Checks: run-wide numbers 1..4; each L3 holds only its own turns with its own
     events (exact message lists); each tail's "# New events" holds only its own event; `Turns(c1)` = {1,3},
     `Turns(c2)` = {2,4}; events' `afterTurn` per conversation (0,1 and 0,2). End c1 + `Maintain`: one fold call whose
     messages hold c1's lines and none of c2's; summary 1-3; c1 folded through 3, c2 still 0. c2's turn 5: L2 has
     "Turns 1-3: Summary.", L3 still c2's turns 2 and 4, and the tail retrieves c1's folded turn 1
     ("- Turn 1: (Rain starts.) Aela: ... Hulda [none]: ...").
   - `tests/hosted/HostedFoldingTests.cpp:304` "A stop between two ended conversations in one Maintain keeps the first
     fold, and the next Maintain folds the second": a test-local `StopAfterEmbed` backend (delegates to
     `ScriptedBackend`, which is `final`) requests the stop after the first fold's history embed, i.e. after c1's fold
     has landed and before c2's fold call. `Maintain` returns false; one summary (1-1), c1 folded through 1, c2 at 0;
     the next `Maintain` makes one call and folds c2 (summary 2-2).
   - Both pin existing behaviour and passed on first run (no RED possible), as the review expected from reading the code.
4. **Damaged stored turn is a StoreError.**
   - `src/store/RunStore.cpp:846-879` (`HostedTurns`): after the observation-is-object check, checks the actors core
     exactly as `HostedWorld::Observe`'s core (`actors` array; each an object with non-empty string `id` and `name`,
     boolean `is_player`, optional string `bio`; unique ids; exactly one player), then that the turn's speaker is one
     of the actors. Faults are `StoreError` "'<file>': hosted turn <n> column 'observation' ..." or
     column 'speaker_id'. Added `<set>`, `<string>` includes.
   - `src/store/RunStore.h:169-172` comment; `src/turn/HostedSession.cpp:428-430` comment in `Entries` (the
     `MORPHEUS_REQUIRE` stays as a true invariant).
   - Test `tests/store/HostedStoreTests.cpp:457` (raw SQL `UPDATE hosted_turns SET observation = ?`, 11 sections: no
     actors, actors not an array, a non-object actor, no player, two players, empty id, non-string name, missing
     is_player, non-string bio, repeated id -> 'observation'; speaker not among the actors -> 'speaker_id').
     RED: 22 of 22 assertions failed (no throw). GREEN: 22 passed.
   - Test-helper adjustment (store tests, not Session/turn/replay): `TestHostedTurn`'s observation was
     `{"actors": []}`, now Hulda (npc-1, the speaker) and Aela (player) (`HostedStoreTests.cpp:101-102`); the UTF-8
     commit test no longer overrides the observation with `{"note": "fine"}` (it now reads back a valid one).
5. **`Session::Load` refuses a hosted run.**
   - `src/turn/Session.cpp:109-115`: right after `RunStore::Open`, `kind != Owned` is `DataError`
     "run '<file>' is a hosted run, not a run of world '<id>'". `src/turn/Session.h:78-79` comment.
   - Test `tests/turn/SessionTests.cpp:403` "Load refuses a hosted run's file before anything else reads it": a
     `CreateHosted` file loaded with a world whose id is `hosted` and with `mini`; no embedding made.
     RED: `ContractError` "StateAt: no snapshot for turn 0" for the `hosted` world and the old world-id message for
     `mini` (3 of 4 assertions failed). GREEN: 4 passed.
6. `src/store/RunStore.h:158-159`: `ConversationEvents` also names the StoreError for an event that is not an object
   with a string `text`.
7. `src/turn/HostedWorld.h:121-125` and `src/turn/HostedWorld.cpp:536-537`: a parse also fails for numbers the parser
   cannot hold (`1e999`), not only lone surrogates. The hosted spec's matching `Adjudicate` line (:40) and the
   architecture spec's `Adjudicate` sentence say the same.
8. Architecture spec hosted section (`2026-10-27-morpheus-cpp-architecture-design.md:177-179`):
   - "R5's three operations" -> "the three operations over it: `Observe`, `ProposalGrammar` (with `ProposalSchema`)
     and `Adjudicate`";
   - participants `- <name> (<id>): <bio>`, "or `- <name> (<id>)` for one without a bio";
   - L3 user message "or the cue `(<speaker> speaks.)` when a turn has neither";
   - the byte-for-byte stable-prefix sentence now excepts a join or leave or a fold in between (summaries are run-wide:
     a fold anywhere changes every open conversation's system message from L2 on; L0, L1 and participants stay);
   - also: reopen order (below), and the store paragraph (:163) notes the actors-core check on read.
   - Hosted spec section 6 gained a StoreError line (damaged saved data incl. the actors core) and the
     `Session::Load` DataError.
9. **Reopen order.**
   - `src/turn/HostedSession.cpp:103-116`: `SyncLore` runs before `ReplaceHostData`, as `Session::Load`
     re-embeds before anything else changes; comment says a failed open leaves the stored copy and hash unchanged.
     `HostedSession.h:64-69` (`Open` comment) and hosted spec section 1 (:57) updated.
   - Test `tests/hosted/HostedSessionTests.cpp:97` "A reopen whose changed lore cannot be embedded leaves the stored
     host data and its hash unchanged": reopen with Hulda's lore changed on a `ScriptedBackend` with
     `SetEmbedLimit(20)` (its Embed throws) -> `DataError` "host data: lore entry 'Hulda' cannot be embedded"; then
     `RunStore::Open` shows the original host data and hash. RED: both CHECKs failed (stored copy was the changed
     data and hash). GREEN: 3 passed.

## Commands and results

- Note: Catch2 splits a test spec at commas; tests whose names contain a comma were run with `\,`.
- RED: `./build/tests/bin/Debug/morpheus_tests.exe "<name>"` for each new test after `cmake --build --preset tests`:
  items 4, 5, 9 failed as above; items 2 and 3 passed (behaviour pins).
- GREEN: the same six runs all passed (9, 39, 15, 3, 4, 22 assertions).
- `ctest --preset tests`: 100% tests passed out of 505 (499 + 6 new), 3 `[server]` skipped.
- `cmake --build --preset asan`, `ctest --preset asan`: build clean (no warnings); 100% tests passed out of 505, 3 `[server]` skipped (234.97 s).
- `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_play morpheus_probe morpheus_tests`:
  exit 0, no warnings; morpheus.exe, morpheus_play.exe, morpheus_probe.exe, morpheus_tests.exe built.
- Byte checks (`LC_ALL=C grep -c $'[\x80-\xFF]'` and `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'`) on all 15
  changed files: 0 and 0 for each. No backslash-u / backslash-x escapes in the added lines (none intended).
- `tasklist | grep -i -E "morpheus|ctest"`: no output (grep exit 1): nothing left running.

## Files changed

src/store/RunStore.cpp, src/store/RunStore.h, src/turn/HostedSession.cpp, src/turn/HostedSession.h,
src/turn/HostedWorld.cpp, src/turn/HostedWorld.h, src/turn/Session.cpp, src/turn/Session.h,
tests/hosted/HostedFoldingTests.cpp, tests/hosted/HostedSessionTests.cpp, tests/hosted/HostedTurnTests.cpp,
tests/store/HostedStoreTests.cpp, tests/turn/SessionTests.cpp,
docs/superpowers/specs/2026-11-16-hosted-world-design.md, docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md

## Deviations and concerns

- Store test helper changed (item 4): the store now refuses the observations two store tests used; noted above.
- The store's actors-core check duplicates `HostedWorld`'s `ReadActors` rules (store sits below turn and cannot call
  it). If the core ever changes, both must change; the comments in RunStore.h/.cpp name `HostedWorld::Observe`.
- Items 2 and 3 tests could not be RED (they pin behaviour the review judged correct).
- The new `StopAfterEmbed` test backend relies on `Maintain`'s only embeds being a landed fold's history (true in D1).

## Follow-up: one reader of the actors core (commit 01f6643)

The coordinator asked that the actors-core rules live in one place (concern 2 above).

- `src/core/HostedRecord.h` / new `src/core/HostedRecord.cpp` (added to `src/core/CMakeLists.txt`): `core::Actor`
  (moved from `turn`), `core::ObservationFault` (pointer, problem), `core::ActorsCore` (actors read whole and every
  fault) and `core::ActorsCore ReadActors(const nlohmann::json& observation)`. The body is `HostedWorld.cpp`'s former
  anonymous `ReadActors`, moved verbatim apart from returning the struct. It never throws.
- `src/turn/HostedWorld.{h,cpp}`: the local `Fault`, `ReadActors` and `turn::Actor` are gone. `Observe` appends
  the schema's errors to `core::ReadActors`' faults, and `ActorsOf` throws on the core's faults, both through the
  unchanged `ObservationError`, so every `DataError` message is the same as before. Callers were updated to
  `core::Actor`; there is no alias (`HostedWorld.h`, `HostedWorld.cpp`, `HostedSession.cpp`). The unused `<map>`
  include was dropped.
- `src/store/RunStore.cpp` (`HostedTurns`): my duplicated check is replaced by `core::ReadActors`. A fault is
  `StoreError` "'<file>': hosted turn <n> column 'observation' does not hold the actors core: at <pointer>:
  <problem>; ...". The speaker check is unchanged: column 'speaker_id', same message. `<set>` became `<algorithm>`.
  The wording of the 'observation' StoreError changed from my first commit (it now names each fault's pointer). The
  file, turn and column are still named, and the tests check only those, so they pass unedited.
- Comments: `RunStore.h` (`HostedTurns`) and `HostedWorld.h` (`ActorsOf`) name `core::ReadActors`. The architecture
  spec's store paragraph says the actors core has one reader and which error each caller turns its faults into.
- No test file changed. `cmake --build --preset tests` and `ctest --preset tests`: 100% tests passed out of 505
  (3 `[server]` skipped). `cmake --build --preset asan` and `ctest --preset asan`: 100% tests passed out of 505. Both
  builds were clean with no warnings. I did not rebuild the dev targets for the follow-up, since the request named
  only `tests` and asan.
- Byte checks on the 9 changed files: 0 and 0 for each. There are no backslash escapes.
- `tasklist | grep -i -E "morpheus|ctest"` printed nothing: nothing is left running.
