# Changelog

The morpheus_server API's versions (`api_version` on every response). A breaking change bumps the major version and
gets an entry here with a migration note.

## API 1.0.0 (unreleased)

The first version (design: `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`).

- `GET /health`; `POST /v1/runs/{run_id}/open`; `POST /v1/runs/{run_id}/conversations`;
  `POST /v1/runs/{run_id}/conversations/{cid}/turn` (Server-Sent Events: `action`, `token`, `sentence`, then `done` or
  `error`), `/events`, `/participants`, `/end`; `GET /v1/runs/{run_id}/conversations/{cid}`; `POST /v1/tokenize`;
  `POST /v1/embeddings`.
- Errors: 400 (a malformed request, naming the field), 404, 409 (a run not opened in this process), 422 (the
  observation and player text do not fit the tail), 503 (a llama-server unreachable), 500.
- `speaker: "decide"` is refused with 400 until a later version.
- Every answer is JSON with `api_version`, those cpp-httplib makes itself included (OPTIONS or an unknown method, a
  malformed request, an exception that escaped a handler: `{"api_version", "error": {"status", "message"}}`).
- A turn stopped by the server's shutdown ends its stream with `error` 503 ("the server is stopping"); every stream
  ends with `done` or `error`, except for a client that disconnected.
