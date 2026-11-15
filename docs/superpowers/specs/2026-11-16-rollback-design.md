# Rolling a hosted run back (sub-project E2) - Design

Date: 2026-11-15. Source: the rollback part of requirement R6 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` ("Reloading an older save: an endpoint to roll a run back to a game-time stamp (records after it are hidden, not deleted), because players reload"; accept: "rollback hides later records and restores them on roll-forward"), with the owner's decisions. It builds on D1's hosted runs, D2's `morpheus_server` and E1's per-speaker memory (`docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`).

## Goal

When the player loads a save, the host tells Morpheus the save's game time, and the run looks as it did then: what happened later is hidden, not deleted, and NPCs neither show nor remember it. Loading a later save again brings it back exactly, as long as nothing new was played in between. Playing on from the older save abandons the hidden future for good.

## Owner decisions

- The clock: the host data names a JSON pointer to a number in the observation. Records carry that clock; requests without an observation take the run's latest seen clock.
- Playing on abandons the hidden future: roll-forward works until something new is recorded after a rollback.
- Memories straddling the rollback point are hidden and refolded in the background; a condense hidden by a rollback brings back the summaries it superseded.
- Approach: materialized rollbacks. Rows carry who hid them, a rollback log saves the marks it moved, readers skip hidden and abandoned rows, and a roll-forward reverses a rollback.

## 1. The clock

- `turn::HostData` gains an optional `clock`: a JSON pointer (RFC 6901) into the observation, e.g. `"/game_time/days_passed"`. `HostDataFromJson` reads it (a string; anything else is a `DataError` naming `/clock`); `HostedWorld` checks it is a well-formed pointer.
- With a clock, every observation must hold a finite number at the pointer: `HostedWorld::Observe` reports a fault there ("observation at <pointer>: expected the clock, a number") as it reports any other fault. `Observation` gains `std::optional<double> clock`.
- Without a clock, nothing changes for the host, and a rewind is refused (section 3).
- The run's latest seen clock is the highest clock among its visible records (turns, conversation starts, participant changes, events, ends, shares); none before the first.

## 2. Records and their clocks

Each record keeps the clock of when it happened:

| Record | Its clock |
|---|---|
| A hosted turn | its observation's clock |
| A conversation start | its request's observation clock |
| A participant change (join, leave) | its request's observation clock |
| An event recorded between turns | the run's latest seen clock when recorded |
| A conversation end | the run's latest seen clock when it ended |
| A share (its copies; a pending share's creation and its application) | the run's latest seen clock then |
| An NPC summary (a fold) | none of its own: it follows the turns it covers |

A record written without a clock (a run with no clock, or written before store version 6) has a null clock and is never hidden by a rewind.

### Conversations as history

A conversation's participants and its end flag are state today. To restore them, they become history: a conversation is its start row plus change rows (`conversation_changes`): each holds the participants after the change and whether it ended the conversation, with its clock. A conversation's participants and ended flag are those of its latest visible change row. `StartConversation`, `SetParticipants` and `EndConversation` append rows; the store upgrade turns each existing conversation into a start row and one change row with its current state.

## 3. Rewind

`HostedSession::Rewind(double gameTime)`: the host calls it on every save load with the save's game time (`POST /v1/runs/{run_id}/rewind`). It is idempotent: the same time again changes nothing.

A run is its live records plus at most one open rollback. Rewind:

1. Undoes the open rollback, if any (below).
2. If `gameTime` is below the latest live clock, applies a rollback to `gameTime`.

So a rewind to an earlier time hides, a rewind to a later time restores (up to that time), and a rewind to or past the latest live clock restores everything.

Without a clock in the host data: `DataError` "game_time: the host data names no clock". A `gameTime` that is not finite: `DataError` "game_time: ...".

### Applying a rollback to T (one transaction)

A rollback row is added (`rollbacks`: id, game time, status `open`, the marks it saves). Then, every row below gets `hidden_by` = the rollback's id:

- turns clocked after T;
- events, participant changes and conversation ends clocked after T (a conversation whose end is hidden is open again);
- conversations started after T, with everything in them;
- shared copies made after T (their clock), and pending shares created after T;
- every NPC summary covering a hidden turn (`toTurn` past the last visible turn of its conversation, or, for a condensed summary, past the run's last visible turn), and every condense that holds a summary this rollback hid (a link in `summary_supersessions` names it), whatever its range, repeated until nothing more is hidden (a condense can itself be condensed): a condense is written after what it holds, so after T too (a shared copy made after T, or a fold reaching past the condense's own end when conversations interleave).

And:

- **Superseded summaries come back:** a summary is shown when it is visible and not superseded by a visible summary. `superseded` becomes a link from the summary to each condense that replaced it (a summary may be condensed again while a rollback hides its first condense), so a hidden condense shows again the summaries it replaced, and they in turn may be hidden if they cover hidden turns (the rule applies down the chain).
- **Applied shares are pending again:** a pending share applied after T is marked unapplied by this rollback; its copies are hidden (above), and Maintain applies it again when the teller's memory is complete.
- **Marks move back, saved first:** the rollback saves every conversation's `folded_through`, every NPC's remembered-through mark, and every share's applied state (applied, its applied clock, unapplied by). Then, per conversation with last visible turn `cut`:
  - `folded_through` = min(saved, `cut`);
  - each NPC's mark = min(saved, `cut`), and, for each of its own hidden fold summaries of that conversation (conversation set, not a shared copy, not a condense) that covers a visible turn, at most that summary's `fromTurn` - 1.
  
  The background catch-up (E1) then refolds what each NPC is missing. Condenses are not used to move marks: a hidden condense brings back its superseded summaries, which keep what they covered.
- **Refolds belong to the rollback:** every summary written while a rollback is open (catch-up folds, condenses, share copies) carries `made_in` = the rollback's id.
- The retrieval index is synced in full after the transaction.

### Undoing the open rollback (one transaction)

Rows with `hidden_by` = its id are shown again; summaries with `made_in` = its id are deleted (with their embeddings); the saved marks and share states are restored (so a share unapplied by the rollback is applied again, and one applied while it was open is pending again); the rollback's status becomes `undone`. The index is synced in full.

### Playing on abandons the hidden future

The first new record while a rollback is open (a turn committed, events recorded, a conversation started, participants changed, a conversation ended, a share made) first abandons the rollback, in one transaction: its status becomes `abandoned`; rows hidden by it stay hidden for good (readers skip abandoned rollbacks' rows like any hidden row); summaries `made_in` it become ordinary (`made_in` cleared); unapplied shares become pending (not applied, their applied clock cleared); its saved marks are dropped. A later rewind has nothing of it to restore.

Background upkeep (Maintain) never abandons: its writes are tagged `made_in`.

### Turn numbers

Turn numbers stay run-wide and unique: the next turn is one past the highest number ever stored, hidden and abandoned turns included. Everything else that reads "a conversation's latest turn" (events' `afterTurn`, L3, catch-up) reads visible turns only.

## 4. Store version 6

- `hidden_by INTEGER NOT NULL DEFAULT 0` on `hosted_turns`, `conversation_events`, `conversations`, `conversation_changes`, `summaries`, `pending_shares`.
- `clock REAL` (null allowed) on `hosted_turns`, `conversation_events`, `conversations`, `conversation_changes`, `summaries` (shared copies), `pending_shares` (created).
- `summaries`: `superseded` is dropped; new `made_in INTEGER NOT NULL DEFAULT 0`. New `summary_supersessions (summary_id, condensed_id)`: one row per summary a condense replaced. A summary is superseded when a link names a visible summary.
- `pending_shares`: `applied INTEGER NOT NULL DEFAULT 0`, `applied_clock REAL` (when it was applied; null in a run with no clock), `unapplied_by INTEGER NOT NULL DEFAULT 0`; applied shares stay as rows instead of being deleted. A share is pending when it is not applied, or unapplied by the open rollback.
- `conversations` keeps `id`, `position`, `folded_through`, `clock`, `hidden_by`; `participants` and `ended` move to `conversation_changes (id, conversation_id, clock, participants, ended, hidden_by)`.
- New `rollbacks (id, game_time, status, saved_marks)`: status `open`, `undone` or `abandoned`; `saved_marks` is JSON: every conversation's folded_through, every NPC mark and every share's applied state.
- Upgrade from version 5 in one transaction: the new columns (clocks null), a change row per conversation from its current participants and ended flag, a supersession link for each `superseded` summary (to its condense: the actor's next condensed summary by id; if none can be found, the upgrade fails as damaged data), applied shares kept (version 5 deleted them, so none exist to keep). Older versions upgrade through 5. Owned runs gain the columns only.
- Readers of hosted rows skip hidden rows (`hidden_by` not 0) unless they are the tests' and the debugging route's "all rows" readers.

## 5. Components

- `turn::HostedWorld`: the clock pointer, `Observation::clock`.
- `store::RunStore`: the version 6 schema and upgrade; visible-only hosted readers; clocks on writes; `ApplyRollback`, `UndoRollback`, `AbandonRollback`, each one transaction; `OpenRollback()`; `LatestClock()`; `NextHostedTurn()` (one past every turn ever stored).
- `turn::HostedSession`: `Rewind`; abandoning before every new record; `made_in` on upkeep writes while a rollback is open; a full index sync after a rewind.
- `server`: `POST /v1/runs/{run_id}/rewind` with `{game_time}` answers `{rewound, hidden_turns, restored_turns, rollback}` (`rollback`: `{game_time}` of the open rollback after the call, or null). A rewind is an ordinary job on the run's worker. `API_VERSION` becomes `1.2.0` (only additions: the host data's `clock` is optional).

## Errors

- No clock, a `game_time` that is not a finite number, an observation without the clock: `core::DataError` naming the field (400).
- Damaged rollback data (a rollback row whose saved marks are not valid JSON, a `hidden_by` naming no rollback, two open rollbacks): `store::StoreError` naming the file and the column.

## Tests (written first)

- **R6's acceptance, a new fixture `tests/hosted/fixtures/reload.json`** (format added to the fixtures README): a run with a clock; a conversation over several game times with events, a participant change, a share and an end; a rewind to a middle time: the later turns, events, change, end and share are hidden, and the NPCs' prompts, memories and retrieval lose them; a rewind to the latest time restores everything exactly (turns, memories, participants, the end); a rewind, then a new turn: the hidden turns are abandoned, and a rewind to the old latest time restores nothing. Played on a scripted backend, and over HTTP.
- Straddling memories: a summary over the rollback point is hidden and its visible turns are refolded by the background catch-up; a condense hidden by a rollback shows its superseded summaries again; refolds are deleted on roll-forward and kept when playing on.
- Shares: a share applied after the point is pending again, then applied again; a share made after the point is hidden.
- Turn numbers after abandoning continue past the abandoned ones.
- Store: the version 5 to 6 upgrade (an owned and a hosted run), each transaction, the visible-only readers, damaged rows.
- Errors: no clock, a bad `game_time`, an observation without its clock.
- Server: the route and its answer, `api_version` 1.2.0.
- Live: the fixture on a real model, checking only what does not depend on the model's words.

## Out of scope

- Branches per save (owner decision: playing on abandons the hidden future).
- F: the host's prompt pack. G: `"decide"`, the optional review, an explicit call tag.

## Documentation

- README: the rewind route and when the host calls it; the clock in the host data.
- `CHANGELOG.md`: API 1.2.0.
- The architecture spec (the store's tables, version 6, rollbacks), the per-speaker memory spec (superseded summaries now name their condense), the morpheus_server spec (the route).
- `tests/hosted/fixtures/README.md`: the clock in `open.json`'s format and the reload fixture's format.
