# Changelog

The morpheus_server API's versions (`api_version` on every response). A breaking change bumps the major version and
gets an entry here with a migration note.

## API 1.6.0

Lore scopes and conditions, and the review as a continuation (design:
`docs/superpowers/specs/2026-11-16-host-feedback-design.md`). Only additions to the shapes.

- A host lore entry's optional `scopes` (an array of non-empty strings) and `requires` (`{"pointer", "min"?, "max"?,
  "equals"?}`: a JSON pointer into the observation and at least one comparison; `min` and `max` are numbers,
  inclusive, `equals` any JSON value, compared by JSON equality), and an observation actor's optional `knows` (an array
  of non-empty strings). A lore entry is visible to a speaker on a turn when its `requires` holds on that turn's
  observation (a missing value, or a value of another type, does not hold) and its `scopes` are empty or share one with
  the speaker's `knows`. A lore entry that is not visible to the speaker is not searched; an always-on entry is shown in the
  system message, the decide prompt and the fold prompts only while its `requires` holds on the observation the prompt
  is built from (a quest stage reached changes the system message once). An always-on entry may not have `scopes`: a
  400 at `open`. Host data without the new keys is unchanged and keeps its hash.
- The review becomes a continuation of the speak prompt: the speak call's messages, the first line as an assistant
  message, then one user message (the pack's `review` text, a refusal note for a refused action, the instruction). It
  sees the conversation and keeps the conversation's prompt cache, so a reviewed turn no longer re-reads the whole
  prompt afterwards. The pack's `review` text is now the start of a user message, not a system message, and its
  default is reworded for that. A reviewed turn keeps room for the review in its prompt (the review message,
  `review_max_tokens`, `review_thinking_tokens` and 16 tokens of template), so its transcript budget is smaller by
  that much. The shapes are the same.

## API 1.5.0

The review of a hosted turn (design: `docs/superpowers/specs/2026-11-16-hosted-review-design.md`). Only additions.

- A turn's optional `review` (boolean, default false): the line is checked against the speaking rules by one more model
  call. The line is held back while the review runs and streams after it (its `token` and `sentence` events come
  after the review); on problems the engine asks for the line once more (one respeak) and the second line stands.
  The cost is one review call, sometimes one more speak call, and the conversation's prompt cache: the review prompt is
  standalone and runs on the conversation's slot, so it replaces that slot's cached prompt, and the next call of that
  conversation (the respeak, then the next turn's resolve) re-reads its whole prompt. Measure with that in mind. A `review` that is not a boolean is a 400.
- The `done` event's `timings` gain `review` (the call's telemetry, `consistent`, `problems`, and `error` when the
  review was unreadable) and `respeak` (the second speak call's telemetry), each only when that call ran.
- The prompt pack gains `review` (no variable), the review call's system text; `GET /v1/prompts/default` lists it.

## API 1.4.0

The engine chooses the speaker (design: `docs/superpowers/specs/2026-11-16-decide-speaker-design.md`). Only additions.

- A turn's `speaker` may be `"decide"`: the engine chooses one of the conversation's NPCs present (not the player).
  On a turn with no player text and no new events (recorded since the last turn or sent with it) the last speaker
  is left out while another remains. With one candidate
  it is chosen at once; with more, a short model call chooses. The `action` event's `speaker` names the chosen NPC.
  No NPC participant in the observation is a 400.
- The prompt pack gains `decide` (variable `{player}`), the decide call's instruction; `GET /v1/prompts/default`
  lists it.
- `decide` is reserved: an observation actor with that id is a 400.
- A decided turn's `timings` gain `decide` (the call, and `refused` with the reasons when the model's answer was
  refused and the candidate who spoke least recently was chosen instead).

## API 1.3.0

The host's prompt pack (design: `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`). Only additions; two default texts changed.

- The host data may carry `prompts`: any of `system`, `resolve`, `speak`, `turn_cue`, `summarize`, `condense`, each a
  text that replaces the engine's default for the run. Variables: `system` none; `resolve`, `speak`, `turn_cue`
  `{speaker}` and `{player}`; `summarize` and `condense` `{actor}`. An unknown key, an empty text, a variable the text
  does not take, or a system text over `engine_rules_cap` fails the `open` with 400 naming `prompts/<key>`.
- `GET /v1/prompts/default` answers `{prompts, variables}`: the engine's default texts and the variables each takes.
- The default speak instruction no longer names the player, and the default system text says most turns need no
  action.

## API 1.2.0

Rolling a run back (design: `docs/superpowers/specs/2026-11-16-rollback-design.md`). Only additions.

- The host data may name a `clock`: a JSON pointer to a number in every observation (the game time).
- `POST /v1/runs/{run_id}/rewind` with `{game_time}` answers `{rewound, hidden_turns, restored_turns, rollback}`: the
  run as it was at that game time. Call it on every save load. What came later is hidden, not deleted; a later rewind
  restores it until a new record (a turn, events, a conversation, participants, an end, a share) abandons it.

## API 1.1.0

Per-speaker memory (design: `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`). Two new routes; and a change of behaviour: every NPC now remembers only what it witnessed.

- `POST /v1/runs/{run_id}/memories/share` with `{from, to, conversation_id}` answers `{shared, pending}`: `to` learns
  what `from` remembers of the conversation, as copies marked told by `from`; a share waits (`pending`) until the
  conversation ended and `from`'s memory of it is complete.
- `GET /v1/runs/{run_id}/actors/{actor_id}/memories` answers `{memories: [{conversation_id, from_turn, to_turn, text,
  told_by, superseded}]}`, oldest first.
- A turn's prompt shows the speaker's own memories, not the run's; summaries, retrieval and folding are per NPC.
- A request other than a turn no longer waits for the run's whole background pass: it waits for the current step
  (one NPC's memory of one conversation, or one share), then the pass pauses and resumes after it.
- A turn's `timings.retrieval[].source` is `memory` for a retrieved memory (its `ref` is the summary's id); folded
  turns stay `history` (by turn number) and lore `lore`.

## API 1.0.0 (unreleased)

The first version (design: `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`).

- `GET /health`; `POST /v1/runs/{run_id}/open`; `POST /v1/runs/{run_id}/conversations`;
  `POST /v1/runs/{run_id}/conversations/{cid}/turn` (Server-Sent Events: `action`, `token`, `sentence`, then `done` or
  `error`), `/events`, `/participants`, `/end`; `GET /v1/runs/{run_id}/conversations/{cid}`; `POST /v1/tokenize`;
  `POST /v1/embeddings`.
- Errors: 400 (a malformed request, naming the field), 404, 409 (a run not opened in this process), 422 (the
  observation and player text do not fit the tail), 503 (a llama-server unreachable), 500.
- `speaker: "decide"` is refused with 400 until a later version.
- Lore that cannot be embedded at `open` because the embedding server is unreachable or failed (5xx, a dropped
  connection) is 503, so the host can try again; only a text the server refuses (an HTTP 4xx answer, such as a text
  too long for its context) is 400, naming the lore entry.
- A `run_id` that is a Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`, in any case) is
  refused with 400.
- Every answer is JSON with `api_version`, those cpp-httplib makes itself included (OPTIONS or an unknown method, a
  malformed request, an exception that escaped a handler: `{"api_version", "error": {"status", "message"}}`).
- A turn stopped by the server's shutdown ends its stream with `error` 503 ("the server is stopping"); every stream
  ends with `done` or `error`, except for a client that disconnected.
