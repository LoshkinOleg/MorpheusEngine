# Global constraints (sub-project D2, morpheus_server)

Verbatim from docs/superpowers/plans/2026-11-16-morpheus-server.md. Spec: docs/superpowers/specs/2026-11-16-morpheus-server-design.md.


- Every response, error or not, carries `"api_version": "1.0.0"`; for a stream, in its first event.
- The server listens on 127.0.0.1 only; port 4998 by default (`--port`); `--runs` defaults to `./runs`; `--settings` defaults to `./settings.json`.
- `run_id` is 1 to 64 characters of `[A-Za-z0-9_-]`. A run's file is `<runs dir>/<run_id>.morpheus`.
- Slot 0 is the upkeep slot (`Maintain`: folding, condensing). Conversations get the other slots, least recently used first. With one slot, upkeep and turns share slot 0.
- Statuses: 400 for `core::DataError`, a bad `run_id`, a malformed body or field, and `speaker: "decide"`; 404 for an unknown route or an unknown conversation of an opened run; 409 for a run not opened in this server process; 422 for `turn::TurnError`; 503 for `inference::InferenceError`; 500 for anything else.
- An error body is `{"api_version": "1.0.0", "error": {"status": <int>, "message": <string>}}`.
- Stream events, in order: `action` (`{api_version, speaker, action, refused?}`; `action` is `{id, params}` or null), `token` (`{text}`), `sentence` (`{text}`), then `done` (`{speaker, action, line, turn, events_omitted, timings}`) or `error` (`{status, message}`). SSE wire form: `event: <name>\ndata: <one line of JSON>\n\n`.
- A sentence ends at `.`, `!`, `?` or the ellipsis character (U+2026), then any closing quotes or brackets, then whitespace. Abbreviations are not special.
- The listening socket owns its port alone: `SO_EXCLUSIVEADDRUSE` on Windows, plain `SO_REUSEADDR` elsewhere.
- `RunStore` schema version 4: `hosted_turns` gains `events_omitted INTEGER NOT NULL DEFAULT 0`; version 3 and version 2 files upgrade in place in a transaction.
- `MORPHEUS_BUILD_SERVER` defaults to `PROJECT_IS_TOP_LEVEL`, needs `MORPHEUS_BUILD_LLAMA_SERVER`, never links llama.cpp. A vendored build defines no server target.
- Only `src/llamaserver/LlamaServerBackend.cpp`, `src/server/HttpServer.cpp` and the tests include `<httplib.h>`.
- CLAUDE.md coding rules apply to every line: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for bad data); method bodies commented enough to read top to bottom; a private helper only with more than one caller (local lambdas are fine); class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS and grouped; types and methods PascalCase, locals camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; members initialized at declaration; RAII, no naked new/delete, raw pointers never own; no legacy paths; ASCII only in source comments and docs.
- Warnings are errors: MSVC `/W4 /permissive-`, Linux `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Cast between `std::size_t` and `int` explicitly.
- Tests are written first (Catch2). Fast loop: `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "<tag or name>"`; full: `ctest --preset tests` and `ctest --preset asan` (build `asan` first with `cmake --build --preset asan`).
- Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`. Commit messages end with the implementing model's trailer, e.g. `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` or `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Before reporting, check every changed file with `LC_ALL=C grep -c $'[\x80-\xFF]' <file>` (must print 0) and leave no `morpheus_tests` or `ctest` process running (`tasklist | grep -i -E "morpheus|ctest"`).


## Decisions the plan made

- Tags: the server's tests that need no real llama-server are tagged [hosted-server] (sub-tags [slots], [sentences], [worker], [registry], [api], [stream], [http]). [server] stays the tag of tests that need real llama-servers.
- A version 2 run file upgrades straight to version 4 (the hosted tables already have events_omitted).
- LlamaServerBackend also reads the chat model's id (ChatModelId) for /health.
- Conversations are nested under their run: /v1/runs/{run_id}/conversations/{cid}/...
