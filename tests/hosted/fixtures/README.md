# Hosted fixtures

Host data and requests written from section 5 of the integration requirements. They drive
`tests/hosted/HostedFixtureTests.cpp` (through `test::ReadFixture` in `HostedTestSupport.h`). The host's recorded
fixtures may replace any of these files, as long as they keep the format below.

## Files

- `open.json`: the host data of a run (`turn::HostDataFromJson`): the action `catalog`, the `observation_schema` and
  the `lore`; and, optionally, the `clock`, a JSON pointer to a number in every observation (rollback spec, section 1).
- `observation.json`: an observation: Hulda (0001A6A4) and Saadia (0001B07C) at the inn, and Aela (00000007), the
  player. A turn without its own `observation` carries this one.
- `conversation.json`: a scripted player conversation (events between and with turns, an action with parameters, a
  refused action, a turn without player text).
- `radiant.json`: two NPCs talking for six turns with no player text.
- `invalid_requests.json`: requests that must fail with a `DataError` before any model call.
- `three_actors.json`: a memory fixture (R6's acceptance): Hulda, Saadia and Belethor with Aela, the player. Belethor
  leaves halfway and Hulda tells a secret while Saadia is away; nobody may remember what it did not witness.
- `reload.json`: a memory fixture with a clock (R6's rollback acceptance): a conversation over several game times with
  events, a participant change, a share and an end. A rewind to the middle hides what came after and NPCs forget it; a
  rewind to the end restores it exactly; playing on after a rewind abandons the hidden future.
- `prompt_pack.json`: a memory fixture with a host pack (F's acceptance): the pack replaces the system text, the resolve
  instruction, the speak instruction and the turn cue; every resolve prompt shows the pack's texts and none of the
  replaced defaults.

Every fixture may carry a `description` string; the tests ignore it.

## Conversation fixtures (`conversation.json`, `radiant.json`)

`{"participants": [ids], "steps": [...]}`. The test starts one conversation with `participants` (in this join order)
and plays `steps` in order. A step is one of two kinds.

**An events step**: `{"events": [event objects]}`. The events are recorded between turns (`RecordEvents`). An event is
an object with a required `text` string; other keys (`type`, `action`, `actor`, `succeeded`, ...) are the host's own.

**A turn step**: `{"turn": {...}, "model": {...}, "expect": {...}}`.

- `turn` is the request: `speaker` (required, an actor id), and optionally `player_text`, `events` (event objects
  sent with the turn), `allowed_actions` (action ids; absent means every action of the catalog) and `observation`
  (absent means `observation.json`).
- `model` says what the fake backend replies; it exists only for the fake and is not part of what a host sends.
  `resolve` is the reply to the resolve call (the proposal JSON text, which may be invalid on purpose) and `line` the
  reply to the speak call. Replies are used in step order.
- `expect` is checked against the committed turn and its prompt:
  - `requested` (required, boolean): whether the adjudication accepted an action.
  - `line` (required): the committed line.
  - `listeners` (required, ids): the turn's listener ids, in order.
  - `action` (optional, `{"id", "params"}`): the adjudicated action id and params.
  - `reasons` (optional, strings): the adjudication's refusal reasons, in order.
  - `l3` (optional, strings): the contents of the resolve prompt's L3 messages (every message after the system
    message, up to the stable prefix), in order. A turn is a user message and an assistant message.
  - `new_events` (optional, strings): the texts of the events listed in the prompt's tail under `# New events`, each
    as `(<text>)` on its own line. An empty list checks that the block is absent.

The test also checks that turn numbers count from 1 and that the conversation holds one turn per turn step.

## Invalid requests (`invalid_requests.json`)

`{"participants": [ids], "requests": [...]}`. The test opens two conversations with `participants` (`c1`, open, and
`c2`, ended) and runs each request against a session whose model has no reply, so any model call fails the test.
A request is `{"name", "conversation", "turn", "error"}`:

- `name`: shown when the check fails.
- `conversation`: `"open"` (the open one), `"ended"` (the ended one), or any other string, used as the id as is.
- `turn`: as in a turn step above.
- `error`: a substring the `DataError` message must contain (the field or JSON pointer at fault).

The test also checks that no model call was made and that no turn was committed.

## Memory fixtures (`three_actors.json`, `reload.json`, `prompt_pack.json`)

`{"observation": {...}, "participants": [ids], "steps": [...], "clock", "prompts"}`. `observation` is the fixture's own
observation: every request and every participants change uses it (a step has no observation of its own). `clock`
(optional) is a JSON pointer into it, the host data's clock (`open.json`'s host data with this `clock`); a run without
one cannot be rewound. A turn, leave, join or start step may then carry a `"clock"` number: that step uses a copy of
the observation with the number written at the pointer (its game time). `prompts` (optional) is the host data's pack
(`open.json`'s host data with these `prompts`). The test starts one conversation with
`participants` (in this join order) at the observation as it is, and plays `steps` in order. A step is one of these
kinds.

- **A turn step**: `{"turn": {...}, "model": {...}, "expect": {...}}`, as in conversation fixtures (`turn` with
  `speaker` and optionally `player_text`; `model` with `resolve` and `line`). `expect` is optional and holds
  substrings checked against every message of the turn's resolve prompt:
  - `prompt_absent` (optional, strings): none may appear.
  - `prompt_contains` (optional, strings): each must appear.
- **`{"leave": [ids]}`** and **`{"join": [ids]}`**: a participants change (`SetParticipants`), with the fixture's
  observation.
- **`{"end": true}`**: the conversation ends (`EndConversation`).
- **`{"maintain": true}`**: background upkeep until nothing is left (`Maintain` until it returns true).
- **`{"memories": {"actor", "contains", "absent", "count", "told_by"}}`**: checks the memories of `actor` (an id),
  every summary of it, superseded included. `contains` (optional, strings): substrings that must appear in some
  summary's text; `absent` (optional, strings): substrings that must appear in none; `count` (optional): how many
  summaries it has; `told_by` (optional, `{"<substring>": "<teller id>"}`): for each substring, the first summary whose
  text contains it must be told by that teller.
- **`{"start": [ids]}`**: a new conversation of those participants (in this join order), which later steps use.
- **`{"share": {"from", "to", "conversation", "shared", "pending"}}`**: `to` (an id) learns what `from` (an NPC id)
  remembers of a conversation (`ShareMemories`). `conversation` (optional) defaults to the fixture's first
  conversation; `shared` (optional, a number) and `pending` (optional, a boolean) are checked against the share's
  result when given.
- **`{"events": [event objects]}`**: events recorded between turns of the current conversation (`RecordEvents`), as in
  conversation fixtures.
- **`{"rewind": {"game_time", "rewound", "hidden_turns", "restored_turns"}}`**: the run as it was at `game_time` (a
  number; `Rewind`), as on a save load. `rewound` (optional, a boolean), `hidden_turns` and `restored_turns` (optional,
  numbers) are checked against its result when given.
- **`{"turns": {"count"}}`**: the current conversation has `count` visible turns.
- **`{"conversation": {"ended", "participants", "events"}}`**: checks the current conversation as it is now. `ended`
  (optional, a boolean); `participants` (optional, ids): its participants in join order; `events` (optional, a
  number): how many visible events it has.

The fixture's model replies are used in step order, for resolve and speak calls only. Memory calls (folds and
condenses) are answered by the test's echo responder (`test::MemoryEchoResponder`), which repeats the lines the call
reads, so a memory holds exactly what its NPC witnessed; the responder is part of the test, not of the format.
