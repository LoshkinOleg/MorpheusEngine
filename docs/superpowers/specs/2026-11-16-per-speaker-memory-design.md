# Per-speaker memory (sub-project E1) - Design

Date: 2026-11-15. Source: requirement R6 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions. Sub-project E was split in two: E1 (this spec) is per-speaker memory and sharing; E2 is rolling a run back to a game time. E1 builds on D1's hosted runs (`docs/superpowers/specs/2026-11-16-hosted-world-design.md`) and D2's `morpheus_server` (`docs/superpowers/specs/2026-11-16-morpheus-server-design.md`).

## Goal

In a hosted run every NPC remembers only what it witnessed, and what other NPCs told it. Today a hosted run has one set of run-wide summaries that every prompt shows, and retrieval searches every folded turn. After E1, each NPC has summaries of its own, written as what that NPC remembers; a speaker's prompt shows its own summaries and retrieves only what it witnessed; and the host can make one NPC tell another what it remembers of a conversation.

Morpheus's own game keeps its behaviour byte for byte.

## Owner decisions

- E1 (per-speaker memory and sharing) first, then E2 (rollback to a game time), as two specs and plans.
- Per-actor summaries: a fold writes one summary for each NPC who witnessed the folded turns, covering only the turns it saw, in that NPC's words. The player has none: the host keeps the player's memory.
- The speaker's memories go after the conversation, in the prompt's tail, so the conversation stays a prefix the speakers of one conversation share in their slot's cache.
- Sharing copies an NPC's summaries of a conversation: the host names the teller, the listener and the conversation.
- Structure: a summary scope in the shared memory upkeep (`turn::MemoryUpkeep`). The owned game passes the run-wide scope it has today; hosted runs pass one NPC's.

## 1. The memory model

### Witnesses

A hosted turn's witnesses are its speaker and its listeners (the other participants present when it was played), which D1 already stores with each turn. Only NPCs keep memories: an actor whose `is_player` is true in the turn's observation has none.

### Summaries per NPC

- A summary belongs to one NPC (its actor id) and comes from one conversation. It covers a range of that conversation's turns which the NPC witnessed (`fromTurn`, `toTurn`, run-wide turn numbers) and is written as what that NPC remembers: the hosted fold instruction names the NPC.
- A summary shared to an NPC is a copy of another NPC's summary, marked with the teller's actor id (`toldBy`). It is the listener's own from then on: shown, retrieved and condensed like the rest.
- Condensing an NPC's summaries (when they pass `summary_cap`) writes one condensed summary for that NPC and marks the ones it replaced superseded. Superseded summaries leave the NPC's prompt but stay in the store: a later share of their conversation still finds them, and retrieval still searches them. A condensed summary has no conversation and no teller.

### Fold marks

- A conversation keeps one fold mark, `folded_through` (D1): its turns up to that number have left the shared transcript (L3).
- Each NPC witness of a conversation has a "remembered through" mark for it: the conversation's turns up to that number are in the NPC's summaries. An NPC is behind in a conversation when its mark is lower than the conversation's `folded_through` and it witnessed a turn in between.
- When a turn's fit loop folds the conversation's oldest turns (L3 over its budget), it writes the speaker's summary of them at once (one model call) and moves the conversation's `folded_through` and the speaker's mark. Every other NPC witness of those turns is now behind.
- Before an NPC speaks, its own missing summaries of the conversation are written first, so a speaker never meets a gap between the transcript and its memory.
- `HostedSession::Maintain` (in the background, on the upkeep slot) catches up every NPC that is behind, in open and ended conversations; folds what remains of each ended conversation for every NPC witness; condenses per NPC when its summaries pass `summary_cap`; and applies pending shares (section 2).

### What a speaker never sees

An NPC that left a conversation halfway never gets a line said after it left: not in L3 (section 3), not in its summaries (written only over turns it witnessed), not in retrieval (section 4).

## 2. Sharing

`HostedSession::ShareMemories(fromActorId, toActorId, conversationId)`:

- The conversation must exist (unknown: `DataError` "conversation_id: ..."; the server answers 404, as for every unknown conversation). `from` must have witnessed at least one of its turns as an NPC, and `to` must be a non-empty actor id other than `from`; anything else is a `DataError` naming the field (`from`, `to`).
- `to` gets copies of `from`'s summaries of that conversation, superseded or not, shared copies excluded (an NPC passes on what it saw, not what it was told), in turn order, each with `toldBy = from`. A copy is a snapshot: what `from` condenses later never changes it. Sharing the same conversation from the same teller to the same listener again copies nothing new (no duplicates: the copies are keyed by their source summary).
- While the conversation is open, or `from` is behind in it, the share is stored as pending, and `Maintain` applies it once the conversation has ended and `from` has caught up. The host never tracks fold timing.
- Returns how many summaries were copied now, and whether the share is pending.

## 3. The prompt

The hosted prompt (`context::AssembleHostedPrompt`), most stable first:

1. L0: the hosted system text.
2. L1: the host's always-on lore, then the participants (D1).
3. L3: the conversation's unfolded turns that the speaker witnessed, as D1 renders them. For participants present throughout, that is the whole unfolded conversation, a prefix the conversation's speakers share in their slot's cache.
4. The tail:
   - the speaker's memories: `# What <name> remembers`, then each shown summary oldest first, a shared one prefixed `(told by <teller's name>)`; absent when the speaker has none;
   - retrieved snippets (`# Relevant`);
   - the observation, the new events, the player's line and the instruction (D1).

L2 leaves the system message of hosted prompts: run-wide summaries no longer exist there. The owned prompt is unchanged.

A teller's name is its name in the conversation's participants when it took part, else in the current observation, else its actor id.

### Budgets

- `summary_cap` bounds the speaker's shown summaries (counted as they render in the tail); over it, the speaker's summaries are condensed (the fit loop's condense, now per NPC).
- The tail (`tail_allowance`) holds the speaker's memories too. The fit loop sheds retrieved hits first, then the oldest events (D2), as today; the memories are never shed (they are capped by `summary_cap`, which a hosted run's settings must keep below `tail_allowance`: `HostedSession::Open` checks it and names both keys; the owned game's settings are not affected).
- L3's budget no longer reserves room for L2 in the system message.

## 4. Retrieval

For speaker X, a hosted turn retrieves from:

- the host's lore (D1);
- the folded turns X witnessed: turn entries are filtered by their speaker and listeners;
- X's own summaries, shown or superseded, shared ones included (a new searchable source; a summary is embedded when it is stored).

Nothing X did not witness or was not told is ever returned. The owned game's sources and results are unchanged.

## 5. Components

### `core` and `store`

- `core::Summary` gains `actorId` (empty for the owned game), `conversationId` (empty for the owned game and for a condensed summary), `toldBy` (empty unless shared), `sourceId` (the summary a shared copy was made from; 0 otherwise), `superseded` (false), and `id` (the store's row id; 0 before it is stored).
- `store::RunStore`, schema version 5:
  - `summaries` gains `actor_id`, `conversation_id`, `told_by`, `source_id`, `superseded`; the owned game's rows keep the empty actor.
  - New table `actor_memory (conversation_id, actor_id, remembered_through)`, primary key the pair.
  - New table `pending_shares (id, from_actor, to_actor, conversation_id)`.
  - Methods: an NPC's shown summaries; all of an NPC's summaries (shown and superseded); add a summary; supersede an NPC's shown summaries and add the condensed one, in one transaction; the remembered-through mark (read, move); add, list and remove pending shares; `Summaries()` keeps meaning the owned run's summaries.
  - Upgrade from version 4, in one transaction: the new columns and tables. In a hosted run, each version 4 summary (run-wide) is copied to every NPC who witnessed a turn in its range, with that conversation's id (the conversation of its turns), and the run-wide row is removed; each NPC witness's mark in a conversation starts at the conversation's `folded_through`. An owned run gains the empty columns only. Version 3 and 2 files upgrade through 4.

### `turn::MemoryUpkeep` and `turn::SummaryScope`

- `SummaryScope` (new, `turn/MemoryUpkeep.h`): which summaries a fold and a condense work on. It reads the scope's shown summaries, stores a fold's summary, replaces the shown summaries with a condensed one, and gives the fold and condense instructions. `MemoryUpkeep` reads and writes summaries only through it.
- `Session` passes the run-wide scope: the store's owned summaries and today's `SUMMARIZE_INSTRUCTION` and `CONDENSE_INSTRUCTION`. Its prompts, seeds and records stay byte-identical; no `Session` test or replay is edited.
- `HostedSession` passes one NPC's scope: that NPC's summaries, and hosted fold and condense instructions that name the NPC (`HOSTED_SUMMARIZE_INSTRUCTION`, `HOSTED_CONDENSE_INSTRUCTION` in `turn/EngineText.h`, with `{actor}`).

### `context`

- `HostedInputs` gains `memories` (the speaker's shown summaries with their tellers' names) and loses `summaries`; `AssembleHostedPrompt` renders them in the tail as section 3 says. The owned prompt builder is unchanged.

### `turn::MemoryIndex`

- A summary source (`store::EmbeddingKind::Summary`), embedded when a summary is stored, keyed by the summary's id.
- `Retrieve` takes an optional filter: the speaker's actor id. Turn hits are kept only when the speaker witnessed the turn; summary hits only when the summary is the speaker's. Without it (the owned game), behaviour is unchanged.

### `turn::HostedSession`

- The per-NPC upkeep of section 1: the speaker's fold in the fit loop, the catch-up before a speaker's turn, and `Maintain`'s catch-up, folds, per-NPC condensing and pending shares.
- `ShareMemories` (section 2) and `Memories(actorId)` (every summary of the NPC: shown, superseded and shared, oldest first).

### `server`

- `POST /v1/runs/{run_id}/memories/share`: `{from, to, conversation_id}` -> `{shared, pending}`. 404 for an unknown conversation, 400 for a share the session refuses.
- `GET /v1/runs/{run_id}/actors/{actor_id}/memories`: `{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}`, oldest first. An actor with none: an empty list. For debugging and the host's tests.
- `API_VERSION` becomes `1.1.0` (only additions); `CHANGELOG.md` gets the entry.

## Errors

- A share the session refuses, an unknown actor field, a malformed request: `core::DataError` naming the field (400).
- A hosted run whose `summary_cap` is not below `tail_allowance`: `core::DataError` from `HostedSession::Open` naming both keys.
- Damaged stored memory (a summary row of a hosted run without its actor, a mark of an unknown conversation): `store::StoreError` naming the file and the column.
- Model mistakes stay values: a fold or condense that does not fit falls back as today (`SummarizeFallback`, `CondenseFallback`).

## Tests (written first, Catch2, fake backend)

- **R6's acceptance, a new fixture `tests/hosted/fixtures/three_actors.json`** (format added to the fixtures README): Hulda, Saadia and Belethor talk with the player; Hulda tells the player a secret while Saadia is away; Belethor leaves halfway. Checked:
  - Belethor's prompts and stored summaries never contain a line said after he left (L3, his memories, his retrieval).
  - Saadia's memories hold Hulda's secret only after a share from Hulda, marked told by Hulda; before it, her prompt and retrieval never contain it.
  - Replayed over HTTP too (`morpheus_server`).
- Morpheus's own game: every existing `Session` test and replay passes unedited.
- Store: version 4 to 5 upgrades of an owned and a hosted run (summaries copied to their NPC witnesses, marks set), per-NPC round trips, supersede in one transaction, pending shares, and damaged rows.
- Upkeep: the speaker's fold in a turn and the others caught up in the background; the catch-up before an NPC speaks; per-NPC condensing (superseded summaries kept); a share copied as a snapshot, never duplicated, and a pending share applied once after the conversation ends.
- Prompt: two NPCs taking turns in one conversation get prompts byte-identical up to the tail; the tail's memories and told-by markers; no L2 in a hosted system message.
- Retrieval: the witness filter, and shared memories retrievable only by the NPC they were shared to.
- Settings: a hosted run with `summary_cap` not below `tail_allowance` is refused at `Open`; the owned game's settings are not checked for it.
- Server: the two routes and their errors; `api_version` 1.1.0.
- Live: the three-actor fixture on a real model (`[llama]` in-process and `[server]` over HTTP).

## Out of scope

- E2: rolling a run back to a game time and forward again.
- F: the host's prompt pack (these texts become part of it then). G: `"decide"`, the optional review, an explicit call tag.
- Memories of the player; memories of actors who never took part in a conversation (they witness nothing).

## Documentation

- README: the morpheus_server section's routes and the memory model in short.
- `CHANGELOG.md`: API 1.1.0.
- The architecture spec (hosted runs, the store's tables, version 5) and the hosted-world spec (summaries per NPC replace run-wide summaries; a pointer here).
- `tests/hosted/fixtures/README.md`: the three-actor fixture's format.
