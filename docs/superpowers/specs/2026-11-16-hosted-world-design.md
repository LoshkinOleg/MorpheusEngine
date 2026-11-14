# Hosted world (sub-project D1) - Design

Date: 2026-11-16. Source: requirement R5 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions, and the turn shape of R4 and R8 that the server will expose. Sub-project D was split in two: D1 (this spec) is the engine seam; D2 builds `morpheus_server` on it.

## Goal

Morpheus can run a world it does not own. The host (the Mantella fork) owns the world and its mechanics and sends what Morpheus needs as data:

- once per run: an action catalog, the JSON Schema of its observation object, and optional lore;
- with each request: the current observation, events, and who speaks.

Morpheus assembles the prompt, proposes the speaker's action under a grammar built from the catalog and the actors present, validates it, streams the spoken line, and keeps the run's memory. It never simulates the world: an accepted action is requested, not applied, and its outcome comes back later as an event.

Everything in D1 is engine code in `src/turn` and below, with no HTTP. Morpheus's own game keeps its behaviour.

## Owner decisions

- D1 (this spec, the engine seam) first, then D2 (`morpheus_server`), as two specs and plans.
- A turn names its speaker. Player text is optional, so NPC-to-NPC turns work; R8's `"decide"` waits for a later sub-project.
- The engine reads a fixed generic core of the observation (the actors); the rest is opaque, validated against the host's schema, and shown in the prompt.
- Memory: the transcript layer is the current conversation; its turns fold into run-wide summaries, and turns of this and earlier conversations are retrieved run-wide. Turns store their speaker, listeners and conversation, so per-speaker memory (sub-project E) has the data.
- Texts: the engine ships default hosted texts. The host may send lore at open. The host's full prompt pack is sub-project F.
- Storage: the same `RunStore`, schema version 3, with new tables for hosted runs.
- Structure: a separate `turn::HostedSession` and `turn::HostedWorld`. `Session`'s fit loop, fold and condense move into a shared piece that both sessions call. There is no `OwnedWorld` class: `Session` keeps its behaviour and its tests, which is what R5's acceptance line asks for.

## 1. Components

### `turn::HostedWorld` (`turn/HostedWorld.h`)

R5's three operations over the host data of one run.

- **Construction** from the host data: the action catalog (section 3), the observation schema, and the lore entries. It checks all three and throws `core::DataError` naming the JSON pointer of the first fault. The observation schema must stay within the extended `grammar::Validate` subset (section 4).
- **`Observation Observe(const nlohmann::json& observation) const`** validates the observation and returns it with its parsed core:
  - first the core, which the engine needs whatever the schema says: `actors`, an array of objects each with `id` (a non-empty string), `name` (a non-empty string) and `is_player` (a boolean), and an optional `bio` (a string). Ids are unique and exactly one actor is the player;
  - then `grammar::Validate` against the host's schema; every error is collected;
  - any fault is a `DataError` that names each failing pointer (for example `/actors/2/id`).
  `Observation` holds the original JSON and the parsed actors (`id`, `name`, `isPlayer`, `bio`) in their order.
- **`std::string ProposalGrammar(const Observation&, const Participants&, const std::vector<std::string>& allowed, std::string_view speakerId) const`** builds the JSON Schema of the speaker's possible actions (section 3) and returns `grammar::FromSchema` of it.
- **`Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const`**:
  - parses the model's text (a parse failure is a refusal: the grammar admits text the JSON parser refuses, such as lone surrogate escapes and numbers it cannot hold, like `1e999`);
  - runs `grammar::Validate` against the turn's proposal schema;
  - returns `Adjudication { bool requested; std::string actionId; nlohmann::json params; std::vector<std::string> reasons; }`. A valid action is `requested` with its id and params; otherwise `requested` is false, the reasons are the parse error or the validation errors, and the speaker takes no action.
- `Adjudication`, the hosted turn record and the conversation types live in `core/HostedRecord.h` (namespace `core`), because `store` stores them and sits below `turn`.

### `turn::HostedSession` (`turn/HostedSession.h`)

One open hosted run, the counterpart of `turn::Session`. Every backend call happens on the caller's thread.

**D1's contract:**
- One `HostedSession` per run file. Two sessions on one file are not supported.
- It is not thread-safe: its caller serializes every call on a run, turns of different conversations included. `Maintain` runs between them and stops through its stop token when the next call is due.
- D1's contract: every model call uses the backend's default slot (`GenerationSpec::slot` is left at -1). `morpheus_server` (D2) gives each conversation its own slot, slot 0 for folding.
- D1 left choosing a slot per conversation and any concurrency across conversations to D2 (`morpheus_server`), which added them (one worker thread per run, a slot per conversation) without changing this API.

- `static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings, std::uint64_t seed, inference::LlmBackend& backend)` creates the run if the file does not exist, or reopens it:
  - the same host data (equal content hash) reopens as is;
  - with changed host data (a mod update), changed lore is re-embedded, as edited lore is in Morpheus's own runs, and only then does the new host data replace the stored copy, so an open that fails leaves the stored host data and its hash unchanged;
  - a file that holds an owned run is a `DataError`.
- `std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation)` returns a new conversation id. Every participant must be an actor of the observation; their bios are taken from it and stored, in this order.
- `void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)` adds events to an open conversation without a turn. Every event recorded since the conversation's last turn joins its next turn's tail (section 6 for the overflow, as D2 changed it).
- `void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined, const std::vector<std::string>& left, const nlohmann::json& observation)` adds participants (bios from the observation) at the end of the join order, and removes those who left.
- `std::optional<HostedTurn> RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks, std::stop_token stop)` plays one turn (section 2). It returns the committed turn, or nullopt when `stop` cancelled it.
- `void EndConversation(std::string_view conversationId)` marks the conversation ended. Its remaining turns are folded by the next `Maintain`.
- `bool Maintain(std::stop_token stop)` is between-turn upkeep, through the shared memory upkeep: it folds the turns of ended conversations into summaries, condenses summaries over their cap on the way, and indexes folded turns for retrieval. A condense needs the hosted system message, so with no ended conversation to fold, an over-cap summary (after a smaller `summary_cap`) is condensed by the next turn's fit loop. It returns false when cancelled; it can simply run again.
- Readers for the tests and D2: `Conversation(id)`, `Turns(conversationId)`, `Summaries()`, `LastAssembly()`.

`HostData` holds the catalog, the observation schema (both JSON) and the lore (`core::LoreEntry` list); `HostedWorld` is built from it.

`HostedTurnRequest` holds `conversationId`, `speakerId`, an optional `playerText`, `events` (JSON objects), the `observation`, and an optional `allowedActions` (ids; absent means every action of the catalog).

`HostedCallbacks` holds `onAction` (called with the `Adjudication` after the resolve call, before the line starts) and `onToken` (the line's pieces as they are written).

### Shared memory upkeep

`Session`'s fit loop (`AssembleFitting`), `FoldOldest` and `Condense` move into one piece in `src/turn` that both sessions call.

- It works on summaries and on transcript entries (a turn number and its rendered text), not on `core::TurnRecord`, so `Session` passes its turns and `HostedSession` its conversation's turns.
- `Session` changes only by calling it. Every existing `Session` test passes unchanged.

### Smaller changes

- **`context`:** the prompt builder takes the hosted layer set (section 2) beside the owned one. Where it takes rendered transcript entries instead of `TurnRecord`s, tests that compare exact prompt text are updated and say so.
- **`grammar::Validate`:** it accepts more keywords for schemas that are only validated (section 4). `FromSchema` refuses them as today.
- **`EngineText`:** the default hosted texts (section 2).
- **`RunStore`:** schema version 3 (section 5).

## 2. A hosted turn

`RunTurn` does these steps in order:

0. **Check the request.**
   - The conversation must exist and be open.
   - The observation must pass `Observe`.
   - The speaker must be a participant, present in the observation, and not the player.
   - Every allowed action id must be in the catalog, and at least one action must be left after the actor scopes are applied (section 3).
   - A fault is a `DataError` naming the field. A player text with no room in the tail allowance is a `TurnError`, as in `Session`.
   - The request's events are added to the conversation before the turn, in order.
1. **Retrieve.** The query is the player text, the speaker's name and the previous line. Hits come from lore and from folded turns of the whole run (this and earlier conversations).
2. **Resolve.** The assembled prompt plus the resolve instruction runs under `ProposalGrammar` with a small token cap.
   - `Adjudicate` marks the action requested or refused.
   - A refused action is logged with its reasons and the turn goes on with no action.
   - `onAction` fires.
3. **Speak.** The resolve prompt, the resolve output as the model wrote it, and the speak instruction give the spoken line.
   - The prompt extends the resolve prompt, so nearly all of it is cached.
   - There is no grammar. The line streams through `onToken` with its own token cap.
4. **Commit.** The hosted turn record holds:
   - the turn number (run-wide, shared with summaries' turn ranges) and the conversation id;
   - the speaker id, and the listeners (the other participants present);
   - the player text, the events added with this request, and the observation;
   - the resolve output exactly as the model produced it, and the adjudication;
   - the line and the timings.

There is no review call, no dice, no state and no snapshot. Thinking, sampling, the context size and the budgets come from `core::Settings`, as in `Session`. A cancel at any step commits nothing.

### The prompt

The layers, most stable first, so a new turn adds to the end of the cached part:

| Layer | Content |
|---|---|
| L0 system | The default hosted system text: voicing characters in a world the host simulates, and the output rules |
| L1 world | The host's always-on lore entries |
| Participants | Each participant's name and bio, in join order. A join or a leave changes the prompt from this layer on |
| L2 summaries | Run-wide summaries of folded turns |
| L3 conversation | This conversation's unfolded turns and events in order: `Aela: ...`, `Hulda [follow]: ...`, `(Hulda is now following Aela.)` |
| Tail | Retrieved snippets; the observation as compact JSON; the new events; the player text; the instruction |

- In L3, a turn shows the player text (under the player's name, when present), then the speaker's line with the requested action in brackets; a refused action shows no bracket.
- Events show their `text` in parentheses.

### Default hosted texts

`EngineText` gains the hosted system text, the resolve instruction and the speak instruction. They use named variables (`{speaker}`, `{player}`) filled by the engine. They are generic: no Skyrim knowledge. Sub-project F makes them replaceable by the host.

### Folding

- When the conversation's L3 is over its budget, the oldest turns fold into a summary through the shared upkeep, as in `Session`.
- `EndConversation` marks the conversation ended; the next `Maintain` folds its remaining turns and indexes them. D2 runs `Maintain` in the background after an end, which is R4's asynchronous fold.

## 3. The action catalog

The host sends the catalog at open:

```json
{"actions": [
  {"id": "none"},
  {"id": "follow", "params": {}},
  {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
  {"id": "give_item", "params": {"item": {"type": "string", "maxLength": 48}}},
  {"id": "end_conversation"}
]}
```

- `id` is a non-empty string, unique in the catalog. `params` may be absent, which means no parameters.
- Parameter types:
  - `{"type": "actor", "scope": s}`. Scope `present` is every actor of the observation; `present_non_player` leaves out the player; `participant` is the conversation's participants present in the observation. The speaker is never a target.
  - `{"type": "string", "maxLength": n}` with an optional `minLength`.
  - `{"type": "integer", "minimum": a, "maximum": b}`.
  - `{"type": "enum", "values": [...]}` of strings.
- Anything else is a `DataError` at open, naming the pointer.

**The proposal schema** for one turn:
- a `oneOf` over the allowed actions, discriminated by `"action"`;
- each branch `{"action": <const id>, "params": {...}}` with every parameter required and no extra keys;
- an actor parameter is an `enum` of the actor ids its scope gives, deduplicated;
- an action whose actor enum would be empty is left out for that turn.

The model writes `{"action":"attack","params":{"target":"0001A6A4"}}` (compact). The turn result gives it back as `{id, params}` (the requirements' section 5.3).

## 4. Observations and the extended `Validate`

- The host sends the observation's JSON Schema at open. `HostedWorld` checks it against the extended subset, so an unsupported keyword fails at open, not at turn time.
- The extended subset is everything `FromSchema` supports, plus:
  - `{"type": "number"}` (any JSON number), with optional `minimum` and `maximum`;
  - `{"type": "boolean"}`;
  - object properties that are not in `required`, which are optional;
  - `additionalProperties: true` (extra keys allowed and not checked);
  - `{"type": "array"}` without `maxItems`;
  - `{"type": "string"}` without `maxLength`.
- `FromSchema` still refuses all of these with a `DataError`, because it must bound the grammar.
- Errors keep `Validate`'s form: a JSON pointer and a message naming the expected shape.

**Events** are JSON objects with a required `text` (a string). Their other fields (`type`, `actor`, `succeeded` and so on) are stored as they are. Joins and leaves go through `SetParticipants`, not events.

## 5. Storage

`RunStore` moves to schema version 3. (D2 later moved it to version 4: `hosted_turns.events_omitted`, section 6.)

- **Meta** gains `kind`, owned or hosted.
- A hosted run stores its host data (catalog, observation schema, lore) and its content hash. Its world id is `hosted`.
- **New tables:**
  - `conversations`: the id, the participants with their bios in join order, whether the conversation is ended, and the last turn folded so far (summary ranges of interleaved conversations may overlap, so each conversation tracks its own folding);
  - `hosted_turns`: the record of section 2, step 4;
  - `conversation_events`: events recorded between turns, with their order and the turn they follow.
- **Shared tables:** summaries and embeddings serve both kinds of run. Turn numbers run across the whole run.
- **Upgrade:** a version 2 file is upgraded in place on open. The new tables are added, `kind` is set to owned, and the version becomes 3; the existing data is not touched. Any other version is refused as today.
- The run's file is the caller's choice. D2 maps the host's `run_id` to a file.

## 6. Errors

- **Host data is a `core::DataError` naming the JSON pointer:** a malformed catalog, observation schema, observation, event or participant list, an unknown speaker, conversation or allowed action, and an owned run opened as hosted. D2 answers these with HTTP 400.
- **A player text that does not fit is a `turn::TurnError`.**
- **Events recorded between turns that do not fit leave the prompt, oldest first.** This replaced D1's `TurnError` in sub-project D2 (`docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, section 3): the newest events that fit stay in the tail, the turn records how many it left out (`eventsOmitted`), its L3 entry leaves out the same ones, and the store keeps them all. The observation, the player text and the instruction must still fit on their own, or the turn is a `TurnError`.
- **Model mistakes are not exceptions.** An action that does not parse or validate is refused and logged with its reasons, and the turn goes on.
- **Backend failures are `inference::InferenceError`** (D2 answers 503).
- **A cancel through `stop`** returns nullopt and commits nothing.
- **Damaged saved data is a `store::StoreError`** naming the file, the turn and the column, including a stored observation without the actors core or a speaker that is not one of its actors. A hosted run's file opened by `Session::Load` is a `DataError`.
- **Engine bugs are `core::ContractError`.**

## Tests (written first, Catch2, fake backend)

**`HostedWorld`:**
- the section 3 catalog gives the expected proposal schema, and its grammar accepts and refuses the right strings;
- each actor scope; the action left out when its target enum is empty; `allowedActions`;
- each fault of the observation core, with its pointer;
- the observation schema checked at open, including an unsupported keyword;
- `Adjudicate` accepts a valid action, and refuses `"intent"` added to `wait`, an unknown action, a target not present, and text that does not parse.

**`grammar::Validate`:** numbers, booleans, optional properties, open objects, and unbounded arrays and strings are accepted; `FromSchema` still refuses each of them.

**`HostedSession`,** driven from fixture files in `tests/hosted/fixtures/` written from the requirements' section 5 (the host's recorded fixtures can replace them later):
- open, reopen with the same host data, reopen with changed host data;
- a scripted conversation: a few turns with events between them, checking each record and the L3 text;
- an NPC-to-NPC turn with no player text;
- the prompt byte-identical across turns of one conversation up to the tail;
- a join that changes the prompt only from the participants layer on;
- folding when the conversation is over budget, and end plus `Maintain` folding the rest and making it retrievable in a later conversation;
- a cancel that commits nothing;
- each request `DataError`.

**Store:**
- a version 3 hosted run round trip;
- a version 2 file upgraded in place, opening as an owned run with its data unchanged.

**`Session`:** every existing test passes unchanged after the shared upkeep moves.

**Model-backed (`[llama]`):** one hosted conversation of a few turns on the real model; every action validates and every line is non-empty.

## Out of scope

- **D2:** `morpheus_server`, HTTP, SSE and `sentence` events, one slot per conversation and any concurrency across conversations (added without changing `HostedSession`'s API; section 1, D1's contract), and `model_id` under llama-server.
- **E:** per-speaker memory, sharing, and rollback to a game time.
- **F:** the host's prompt pack and its layer and variable validation.
- **Later:** R8's `"decide"`, the optional review per turn, and an explicit call tag (R9).

## Documentation

- The architecture spec gains a section on hosted runs: `HostedWorld`, `HostedSession`, the shared upkeep, the hosted layers and store version 3.
- `CLAUDE.md`'s layout line for `src/turn` mentions hosted runs.
