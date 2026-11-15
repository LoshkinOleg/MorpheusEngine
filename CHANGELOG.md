# Changelog

The morpheus_server API's versions (`api_version` on every response). A breaking change bumps the major version and
gets an entry here with a migration note.

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
