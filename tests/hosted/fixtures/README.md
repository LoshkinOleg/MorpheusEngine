# Hosted fixtures

Host data and requests written from section 5 of the integration requirements. They drive
`tests/hosted/HostedFixtureTests.cpp` (through `test::ReadFixture` in `HostedTestSupport.h`). The host's recorded
fixtures may replace any of these files, as long as they keep the format below.

## Files

- `open.json`: the host data of a run (`turn::HostDataFromJson`): the action `catalog`, the `observation_schema` and
  the `lore`.
- `observation.json`: an observation: Hulda (0001A6A4) and Saadia (0001B07C) at the inn, and Aela (00000007), the
  player. A turn without its own `observation` carries this one.
- `conversation.json`: a scripted player conversation (events between and with turns, an action with parameters, a
  refused action, a turn without player text).
- `radiant.json`: two NPCs talking for six turns with no player text.
- `invalid_requests.json`: requests that must fail with a `DataError` before any model call.

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
