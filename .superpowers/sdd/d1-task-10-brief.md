### Task 10: Documentation and full verification

The architecture spec gains a section on hosted runs and its tables follow store version 3; `CLAUDE.md`'s layout lines mention hosted runs and the fixtures. Then every preset is built and tested once more.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the names of Tasks 1-9.
- Produces: documentation only.

- [ ] **Step 1: Update the architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, make these replacements. Each "Replace" text appears once in the file; "with" is the new text, exactly.

1. Replace

   ```text
   | `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings | core |
   ```

   with

   ```text
   | `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings; for hosted runs the host data, conversations, hosted turns and the events between them | core |
   ```

2. Replace

   ```text
   | `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
   ```

   with

   ```text
   | `context` | Layered prompt assembly (the owned resolve prompt and the hosted prompt, on rendered transcript entries), exact token budgets, cache-prefix tracking | core, inference |
   ```

3. Replace

   ```text
   | `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding, session), the worker thread, replay recording | core, store, inference, context, grammar |
   ```

   with

   ```text
   | `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding and the shared memory upkeep, session), hosted runs (`HostedWorld`, `HostedSession`), the worker thread, replay recording | core, store, inference, context, grammar |
   ```

4. Replace

   ```text
   `Validate` checks a value against the same subset and returns its mistakes as data, a JSON pointer into the value and a message each; it compares integers by their written form, so a huge unsigned never equals a negative.
   ```

   with

   ```text
   `Validate` checks a value against the same subset, extended for schemas that are only validated (numbers with optional bounds, booleans, optional keys, open objects with `additionalProperties: true`, strings and arrays without an upper limit, and none of llama.cpp's limits; `FromSchema` still refuses each, and `CheckValidationSchema` checks such a schema without a value), and returns its mistakes as data, a JSON pointer into the value and a message each; it compares integers by their written form, so a huge unsigned never equals a negative.
   ```

5. Replace

   ```text
   | `run_meta` | world id, world content hash, model id, RNG seed, settings |
   ```

   with

   ```text
   | `run_meta` | world id, world content hash, model id, RNG seed, settings, kind (`owned` or `hosted`) |
   ```

6. Replace

   ```text
   | `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |
   ```

   with

   ```text
   | `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |
   | `host_data` | hosted runs: the host's action catalog, observation schema and lore as one JSON document (its hash is `run_meta`'s world hash) |
   | `conversations` | hosted runs: id (`c1`, `c2`, ...), participants with their names and bios in join order, ended, folded through |
   | `hosted_turns` | hosted runs: n (run-wide), conversation, speaker, listeners, player text, the request's events, observation, resolve output as written, adjudication, line, timings |
   | `conversation_events` | hosted runs: events recorded between turns, in order, with the conversation's turn they follow |
   ```

7. Replace

   ```text
   The schema version is 2 (the `digest` column; it also covers `attempt`). A run written with another version is refused, without migration.
   ```

   with

   ```text
   The schema version is 3 (`run_meta.kind` and the hosted tables; version 2 added the `digest` column and covers `attempt`). Every file has every table. A version 2 run is upgraded in place on open: the hosted tables are added and it is marked owned, in one transaction, its data untouched. A run written with any other version is refused.
   ```

8. Replace

   ```text
   ## UI (ImGui docking windows)
   ```

   with

   ```text
   ## Hosted runs

   Morpheus can run a world it does not own (design: `docs/superpowers/specs/2026-11-16-hosted-world-design.md`). The host sends once per run an action catalog, the JSON Schema of its observation and optional lore (`turn::HostData`), and with each request the observation, events and who speaks. Morpheus never simulates the world: an accepted action is requested, not applied, and its outcome comes back later as an event.

   - **`turn::HostedWorld`** checks the host data (a `DataError` naming the JSON pointer of the first fault) and holds R5's three operations. `Observe` validates an observation: first a fixed core the engine reads whatever the schema says (`actors`, each with a non-empty `id` and `name`, `is_player` and an optional `bio`; ids unique; exactly one player), then `grammar::Validate` against the host's schema, which must be in the extended subset; every fault is reported. `ProposalSchema` / `ProposalGrammar` build the speaker's possible actions for one turn: a `oneOf` over the allowed actions in catalog order, discriminated by `"action"`, each `{"action": <id>, "params": {...}}` with every parameter required and no extra keys; an actor parameter is an enum of the ids its scope gives (`present`, `present_non_player`, `participant`), never the speaker, and an action with no one to name is left out. `Adjudicate` parses the model's text (a parse failure is a refusal) and validates it: a valid action is requested, anything else refused with its reasons.
   - **`turn::HostedSession`** is one open hosted run in the same `RunStore` (kind `hosted`, world id `hosted`). `Open` creates the run or reopens it; changed host data (a mod update) replaces the stored copy and re-embeds changed lore; an owned file is refused. Conversations start with participants (their names and bios from the observation, in join order), take events between turns, gain and lose participants, and end. `RunTurn` checks the request (no model call before it passes), retrieves from lore and the run's folded turns, resolves under the proposal grammar, reports the adjudication through `onAction`, speaks on a prompt that extends the resolve prompt (streamed through `onToken`, no grammar), and commits the hosted turn record; a cancel commits nothing. There is no review, no dice, no state and no snapshot.
   - **The hosted prompt** (`context::AssembleHostedPrompt`), most stable first: L0 the hosted system text (`EngineText`, generic, no variable), L1 the host's always-on lore, the participants (`- <name> (<id>): <bio>`, in join order; a join or a leave changes the prompt from here on), L2 the run-wide summaries, L3 the conversation's unfolded turns as user/assistant pairs (the events in parentheses and the player's line, then `<speaker> [<action>]: <line>`; a refused action has no bracket), then the tail: retrieved snippets, the observation as compact JSON, the new events, the player's line and the resolve instruction. Budgets and flags are the owned prompt's; `world_cap` bounds the lore and the participants together. The prompt of a conversation's next turn starts with this turn's stable part, byte for byte.
   - **The shared memory upkeep** (`turn::MemoryUpkeep`) is `Session`'s fit loop, fold and condense, working on summaries and rendered transcript entries (`context::TranscriptEntry`: a turn number, its L3 messages, its fold messages and a one-line fact), so `Session` passes its turns and `HostedSession` its conversation's. During a conversation, an L3 over its budget folds its oldest turns into a run-wide summary; after `EndConversation`, `HostedSession::Maintain` folds every remaining turn (in one call when it fits). Folded turns of every conversation are retrievable history, under their run-wide turn numbers.

   ## UI (ImGui docking windows)
   ```

9. Replace

   ```text
   - **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer.
   ```

   with

   ```text
   - **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer; the hosted layers, their budgets and their stable prefix across turns and joins.
   - **Hosted tests:** `HostedWorld` (the proposal schema and its grammar, actor scopes, allowed actions, each fault of the host data and the observation core, `Adjudicate`), `HostedSession` (open, reopen and host data updates, conversations, turns, cancel, each request error, folding during a conversation and after its end) and the store's version 3 and upgrade, all with the fake backend. Fixtures in `tests/hosted/fixtures/` are written from the integration requirements' data contracts: the host data, an observation, a scripted conversation, a radiant conversation of two NPCs and the requests that must fail; the host's recorded fixtures can replace them. One `[llama]` test plays a hosted conversation on the real model.
   ```

- [ ] **Step 2: Update CLAUDE.md**

In `CLAUDE.md`:

1. Replace

   ```text
   - src/turn: turn orchestration: grammar, resolve and apply, retrieval, folding, session, worker, replay.
   ```

   with

   ```text
   - src/turn: turn orchestration: grammar, resolve and apply, retrieval, folding and the shared memory upkeep (`MemoryUpkeep`), session, worker, replay; hosted runs, a world the host owns and sends as data (`HostedWorld`, `HostedSession`).
   ```

2. Replace

   ```text
   - tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds.
   ```

   with

   ```text
   - tests/: Catch2 tests (every tests/**/*.cpp is compiled in), tests/data: test worlds, tests/hosted/fixtures: hosted-run fixtures written from the host's data contracts.
   ```

- [ ] **Step 3: Check the documentation's bytes and names**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
grep -n "schema version is 2\|without migration" docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
```

Expected: every count printed is `0`; the last `grep` prints nothing.

- [ ] **Step 4: Run the whole fast suite and AddressSanitizer**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 488` (427 + 61), the three `[server]` tests "Not Run (Skipped)".

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `0 tests failed out of 488`, the same three skipped, no AddressSanitizer report.

- [ ] **Step 5: Build every dev target and run the model-backed tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run, with the two model variables exported (Global Constraints): `./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"`
Expected: `All tests passed`; the thinking tests and the `[server][llama]` test skipped as their variables say.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 3`
Expected: exits with 0.

- [ ] **Step 6: Run the consumer check**

Run: `cmake -S cmake/consumer-check -B build/consumer-check && cmake --build build/consumer-check --config Debug && ./build/consumer-check/Debug/consumer_check.exe`
Expected: configures (no new library, no unwanted target), builds, and prints `consumer_check: ok (rolled ..., total ...)`.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md CLAUDE.md
git commit -m "Document hosted runs: HostedWorld, HostedSession, the shared memory upkeep, the hosted layers, store version 3" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git status --short
```

Expected: `git status --short` lists nothing of this plan (an untracked `.superpowers/` or the ignored `settings.json` are not staged and stay so).

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| `HostedWorld` constructed from the host data; checks catalog, observation schema, lore; `DataError` naming the JSON pointer of the first fault | 4 |
| Observation schema within the extended `Validate` subset, checked at open (an unsupported keyword fails at open) | 1 (`CheckValidationSchema`), 4 |
| `Observe`: the core (`actors` with `id`, `name`, `is_player`, optional `bio`; unique ids; exactly one player), then `Validate`; every error collected; `DataError` naming each failing pointer | 4 |
| `Observation` holds the original JSON and the parsed actors in order | 4 |
| `ProposalGrammar(Observation, Participants, allowed, speakerId)` = `FromSchema` of the proposal schema | 4 |
| `Adjudicate`: parse (failure is a refusal), `Validate` against the turn's proposal schema, `{requested, actionId, params, reasons}` | 4 |
| `HostedSession::Open`: create; same host data reopens as is; changed host data replaces and re-embeds changed lore; owned file is a `DataError` | 6 |
| `StartConversation`: new id; participants are actors of the observation; bios stored in order | 6 |
| `RecordEvents` adds events to an open conversation without a turn | 6 |
| `SetParticipants`: joins at the end of the join order with bios; removes those who left | 6 |
| `RunTurn` returns the committed turn or nullopt on cancel | 7 |
| `EndConversation` marks it ended; the next `Maintain` folds the rest | 6, 8 |
| `Maintain`: folds ended conversations, condenses over-cap summaries, indexes folded turns; false when cancelled, can run again | 8 |
| Readers `Conversation(id)`, `Turns(conversationId)`, `Summaries()`, `LastAssembly()` | 6 |
| `HostData`, `HostedTurnRequest` (`conversationId`, `speakerId`, optional `playerText`, `events`, `observation`, optional `allowedActions`), `HostedCallbacks` (`onAction` after resolve before the line, `onToken`) | 4, 7 |
| Shared memory upkeep: fit loop, fold and condense move into one piece working on summaries and transcript entries; `Session` only calls it; every `Session` test unchanged | 3 |
| `context`: the prompt builder takes the hosted layer set; rendered entries instead of `TurnRecord`s, tests updated and saying so | 3, 5 |
| `grammar::Validate` extended; `FromSchema` refuses each | 1 |
| `EngineText`: hosted system text, resolve instruction, speak instruction, with `{speaker}` and `{player}`; generic | 5 |
| `RunStore` schema version 3 | 2 |
| Turn step 0: conversation exists and open; observation passes; speaker participant, present, not the player; allowed ids in the catalog and one action left; `DataError` naming the field; player text too long is `TurnError`; request events added before the turn in order | 7 (and 4 for `ProposalSchema`'s checks) |
| Step 1: retrieval with player text, speaker's name and previous line; lore and folded turns of the whole run | 6 (`SyncEntries`), 7, 8 (later conversation) |
| Step 2: resolve under `ProposalGrammar` with a small cap; `Adjudicate`; refused logged with reasons, the turn goes on; `onAction` fires | 7 |
| Step 3: speak on the resolve prompt plus the resolve output and the speak instruction; no grammar; streamed with its own cap | 7 |
| Step 4: the record (turn number run-wide, conversation, speaker, listeners, player text, request events, observation, resolve output as written, adjudication, line, timings) | 2 (types and storage), 7 |
| No review, dice, state or snapshot; thinking, sampling, context and budgets from `core::Settings`; a cancel commits nothing | 7 |
| The prompt layers L0 / L1 / participants / L2 / L3 / tail, most stable first; L3 shows the player text under the player's name, the line with the action in brackets (none when refused), events in parentheses | 5 (layout), 6 (`Entries`), 7, 9 |
| Folding: over-budget L3 folds through the shared upkeep; `EndConversation` + `Maintain` folds the rest and indexes it | 7 (fit loop), 8 |
| Catalog: `id` non-empty and unique; `params` may be absent; the four parameter types; anything else a `DataError` naming the pointer | 4 |
| Proposal schema: `oneOf` discriminated by `"action"`; each branch with every parameter required and no extra keys; actor enum of the scope's ids, deduplicated, never the speaker; an action with an empty actor enum left out | 4 |
| Turn result `{id, params}` (section 5.3) | 2 (`Adjudication`), 9 (fixtures' `action`) |
| Extended subset: number with optional min/max, boolean, optional properties, `additionalProperties: true`, arrays without `maxItems`, strings without `maxLength`; errors keep `Validate`'s form | 1 |
| Events: objects with a required `text`; other fields stored as they are; joins and leaves through `SetParticipants` | 4 (`CheckEvents`), 6, 7 |
| Storage: meta `kind`; hosted host data with its content hash; world id `hosted`; `conversations`, `hosted_turns`, `conversation_events`; shared summaries and embeddings; run-wide turn numbers | 2, 6 |
| Upgrade: a version 2 file upgraded in place (new tables, `kind` owned, version 3, data untouched); any other version refused | 2 |
| The run's file is the caller's choice | 6 (`Open` takes the path) |
| Errors: host data `DataError` naming the pointer; player text that does not fit `TurnError`; model mistakes not exceptions; backend failures `InferenceError`; cancel nullopt; engine bugs `ContractError` | 4, 6, 7 |
| Tests, `HostedWorld`: section 3 catalog schema and grammar; each scope; empty target left out; `allowedActions`; each core fault with its pointer; observation schema at open with an unsupported keyword; `Adjudicate` accepts and refuses `"intent"` on `wait`, an unknown action, a target not present, unparsable text | 4 |
| Tests, `Validate`: numbers, booleans, optional properties, open objects, unbounded arrays and strings accepted; `FromSchema` refuses each | 1 |
| Tests, `HostedSession` from fixture files in `tests/hosted/fixtures/`: open, reopen same, reopen changed; scripted conversation with events between turns, records and L3; NPC-to-NPC; prompt byte-identical up to the tail; a join changing the prompt only from the participants layer; folding over budget; end + `Maintain` and retrieval in a later conversation; a cancel; each request `DataError` | 4 (fixtures), 6, 7, 8, 9 |
| Tests, store: version 3 hosted round trip; version 2 upgraded in place, opening as owned with its data unchanged | 2 |
| Tests, `Session`: every existing test passes unchanged | 3 (and every later task's full run) |
| Model-backed (`[llama]`): one hosted conversation of a few turns; every action validates; every line non-empty | 9 |
| Docs: architecture spec section on hosted runs (`HostedWorld`, `HostedSession`, shared upkeep, hosted layers, store version 3); `CLAUDE.md` layout line for `src/turn` | 10 |
| Out of scope (D2 server, E per-speaker memory, F prompt pack, `"decide"`, per-turn review, call tag) | not touched |
