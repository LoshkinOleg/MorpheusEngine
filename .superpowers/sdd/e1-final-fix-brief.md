# E1 final fix round: requirements

The final whole-branch review of sub-project E1 (per-speaker memory) is `.superpowers/sdd/e1-final-review.md`; read it for
the file:line detail. The owner decided the open questions (2026-11-15). Make each item its own commit, tests first, in
this order. HEAD at the start: 40ca5e5.

## 1. The version 4 upgrade gives a memory only to its witnesses (I1; owner: fix)

`RunStore::Open`'s version 4 move copies a run-wide summary to every NPC who witnessed any turn in its range. D1 stored a
fold of interleaved conversations (c1's turns 1 and 3 around c2's turn 2) as the range 1..3, so c2's NPCs get c1's memory
and the copy loses its conversation label.

- A version 4 summary's conversation is decided by its two end turns (`from_turn` and `to_turn`): the same conversation
  for both means a fold of that conversation; different conversations mean a condense (run-wide).
- A fold's copies go only to the NPC witnesses of that conversation's turns inside the range, labelled with it. A
  condense's copies go to the NPC witnesses of every turn in the range, with an empty conversation (as now).
- An end turn that does not exist stays the StoreError it is today.
- Update the test at tests/store/HostedStoreTests.cpp (about line 834) that asserts the old outcome: the interleaved fold
  of c1 goes to c1's witnesses only, labelled c1; a separate condense over both conversations keeps an empty label.
- Fix the comments that called the empty label "safer".

## 2. Memory writes sync only what changed (I2)

`SyncHistory` re-reads every turn of the run, re-renders every folded turn and reloads every vector, after every fold,
catch-up fold, condense and share.

- A catch-up fold, a condense and a share change only summaries: they call only `_index.SyncSummaries(...)`.
- The turn index syncs (`SyncEntries` over the folded turns) only when a conversation's `folded_through` moved: a fold
  during a turn, `MarkFolded` in `Maintain`, and `Open`.
- Read each conversation's turns once per sync (today `SyncHistory` reads them twice: once for witnesses, once inside
  `Entries`).
- No behaviour change: every existing test passes; add a test that a catch-up fold, a condense and a share embed no turn
  (the scripted backend counts embedded texts: `EmbeddedTexts()`), while a fold during a turn embeds the newly folded
  turns.
- Leave an incremental index and a cheaper `Maintain` check for later; note them in the report.

## 3. Background folding stops between catch-ups when any request waits (owner decision)

Since D2, only a turn stops a running fold; other jobs (events, shares, participants, GETs) wait for the whole Maintain
pass, which E1 made much longer (one catch-up per NPC who is behind, per conversation).

- `HostedSession::Maintain(std::stop_token stop, std::stop_token yield = {})`: `stop` cancels a running call, as today;
  `yield` asks Maintain to return between steps. Between NPC catch-ups, between conversations, and between pending
  shares, Maintain returns false when `yield` is requested (it resumes next time, from the marks). A call in progress
  finishes.
- `RunWorker`: every queued job requests the maintenance yield; a preempting job (a turn) also requests the stop, as
  today. Keep the existing turn behaviour and tests.
- Tests: a GET queued during a Maintain that has several NPCs to catch up runs after the current catch-up call, not after
  the whole pass (count the fold calls made before the GET's job runs: one, not all), and the pass completes later.
- Update the morpheus_server spec's section 2 ("One worker per run") and the per-speaker memory spec if they describe it.

## 4. Small items from the review's triage

Do the ones `.superpowers/sdd/e1-final-review.md` lists as worth fixing now (it names them); skip the ones it says can
wait, and say which in the report.

## Finally

- `ctest --preset tests`, then `cmake --build --preset asan` and `ctest --preset asan`: both pass.
- `"[turn]"`, `"[replay]"`, `"[context]"`: unchanged (Morpheus's own game).
- Byte checks on every changed file.
