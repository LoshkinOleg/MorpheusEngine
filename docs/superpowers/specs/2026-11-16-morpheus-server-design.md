# morpheus_server (sub-project D2) - Design

Date: 2026-11-16. Source: requirements R1 and R4 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions. D2 builds the server on D1's engine seam (`docs/superpowers/specs/2026-11-16-hosted-world-design.md`: `turn::HostedWorld`, `turn::HostedSession`).

## Goal

The Mantella fork reaches Morpheus over HTTP on localhost. `morpheus_server` is a single process that holds the host's runs, plays hosted turns through `HostedSession`, streams each turn's action and spoken line as Server-Sent Events, and keeps the runs' memory folded in the background. It reaches the models through llama-server only.

## Owner decisions

- Model access: llama-server only (`llamaserver::LlamaServerBackend`). The server never links llama.cpp, so it builds in every top-level preset and its tests run against the fake llama-server.
- Concurrency: a queue per run. One worker thread per run serializes that run's requests; different runs proceed in parallel. Each conversation keeps a llama-server slot of its own, so alternating conversations keep their cached prompts. `HostedSession`'s D1 contract (one caller at a time per run) is kept.
- Event overflow (left open by D1): the tail shows the newest events that fit. The store keeps every event; the turn records how many its prompt left out.
- R8's speaker `"decide"` stays out of D2: the turn endpoint answers it with HTTP 400 until a later sub-project.
- Structure: a library `src/server` in three layers (run workers, a transport-free API, the HTTP adapter) and a thin executable in `tools/server`.

## 1. HTTP API

### Basics

- The server listens on 127.0.0.1 only; port 4998 by default (`--port`).
- Bodies are JSON (UTF-8); a body that is not a JSON object answers 400.
- Every response, error or not, carries `"api_version": "1.0.0"` (semver), in the JSON body and, for a stream, in its first event. That includes the answers cpp-httplib makes itself (a method no route takes such as OPTIONS, a request it refuses, its own 404, an exception that escaped a handler: 500, logged): `HttpServer` gives them the Api's error body. A breaking change bumps the major version and gets a `CHANGELOG.md` entry with a migration note.
- The listening socket owns its port alone (`SO_EXCLUSIVEADDRUSE` on Windows, plain `SO_REUSEADDR` elsewhere; cpp-httplib's default lets a second server share a live port). A port in use makes the server exit at startup with a message naming it.

### Endpoints

Conversations sit under their run: their ids are short and per run (`c1`, `c2`, ...), as D1 makes them. The brief's `/v1/conversations/{id}` paths were suggestions; the host always knows the run.

| Endpoint | Body | Answer |
|---|---|---|
| `GET /health` | - | `{ready, chat_model, embedding_model, slots, api_version}` |
| `POST /v1/runs/{run_id}/open` | `{catalog, observation_schema, lore?, seed?}`: D1's host data (`HostDataFromJson`) and an optional sampling seed (default 0) | `{run_id, created}`. Idempotent: the same host data reopens as it is; changed host data is D1's re-embed-then-replace. |
| `POST /v1/runs/{run_id}/conversations` | `{participants, observation}` | `{conversation_id}` |
| `POST /v1/runs/{run_id}/conversations/{cid}/turn` | `{speaker, player_text?, events?, observation, allowed_actions?}` | A Server-Sent Events stream (below) |
| `POST /v1/runs/{run_id}/conversations/{cid}/events` | `{events}` | `{}` |
| `POST /v1/runs/{run_id}/conversations/{cid}/participants` | `{joined?, left?, observation}` | `{participants}`: the ids in join order |
| `POST /v1/runs/{run_id}/conversations/{cid}/end` | `{}` | `{}`; folding follows in the background (section 2) |
| `GET /v1/runs/{run_id}/conversations/{cid}` | - | `{conversation, turns, events}`: the stored records, for debugging and the host's own tests |
| `POST /v1/runs/{run_id}/memories/share` | `{from, to, conversation_id}` | `{shared, pending}`: `to` learns what `from` remembers of the conversation; added in 1.1.0 (E1) |
| `GET /v1/runs/{run_id}/actors/{actor_id}/memories` | - | `{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}`, oldest first; added in 1.1.0 (E1) |
| `POST /v1/tokenize` | `{content}` | `{tokens}`: the exact count from the chat model |
| `POST /v1/embeddings` | `{input: [texts], purpose: "query" or "document"}` | `{embeddings: [[...]], model}` |

`run_id` is 1 to 64 characters of `[A-Za-z0-9_-]`, and not a Windows device name in any case (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`: its file would name the device).

### The turn stream

`Content-Type: text/event-stream`. Each event is `event: <name>` and one `data:` line of JSON.

- `action`: `{api_version, speaker, action, refused?}`. `action` is `{id, params}`, or null when the proposal was refused. A refused proposal adds `refused`: its reasons. The event is sent as soon as the action has been adjudicated, before the line starts.
- `token`: `{text}`, one per streamed piece.
- `sentence`: `{text}`, sent as soon as a sentence of the line is complete (section 1, "Sentences").
- `done`: `{speaker, action, line, turn, events_omitted, timings}`. The first three are the brief's turn result (section 5.3); `turn` is the committed turn's number; `timings` holds the resolve and speak times in ms and the tokens reused.
- `error`: `{status, message}`, for a failure after the stream has started (an `InferenceError` during the line: status 503). The stream then ends and no turn is committed.

A client that disconnects during a turn stops it through its stop token: `RunTurn` returns nullopt and nothing is committed, as D1 defines. The worker moves on to the next request.

### Sentences

`server::SentenceSplitter` turns the streamed pieces into sentences:

- A sentence ends at `.`, `!`, `?` or the ellipsis character, followed by any closing quotes or brackets, then whitespace. The sentence is everything up to and including those closing characters, trimmed.
- A `.` between two digits (`3.5`) ends nothing. A run of `...` ends a sentence like `.` does.
- Pieces may split words and punctuation anywhere; the splitter keeps the unfinished text until it ends.
- When the line ends, what is left (trimmed, if not empty) is the last sentence.
- Abbreviations ("Mr. Smith") are not special: they end a sentence. This is documented, not handled.

### Errors

Errors before a stream starts are JSON with an HTTP status: `{api_version, error: {status, message}}`.

| Status | When |
|---|---|
| 400 | A malformed body or field: D1's `core::DataError` (its message names the JSON pointer), a bad `run_id`, a missing or unknown field type, or `speaker: "decide"` ("not supported yet"); a lore entry the embedding server refuses (an HTTP 4xx answer for it, such as a text too long for its context: `inference::InputRefused`, a DataError naming the entry) |
| 404 | An unknown route or an unknown conversation of an opened run |
| 409 | A run used before its `open` in this server process (its host data comes with `open`) |
| 422 | `turn::TurnError`: the observation and player text alone do not fit the tail |
| 503 | `inference::InferenceError`: a llama-server is unreachable or failed (a 5xx answer, a dropped connection, an answer that cannot be read), embedding lore at `open` included, so the host can try again |
| 500 | Anything else (`core::ContractError`, `store::StoreError`); logged with its message |

## 2. Runs, workers and slots

### Runs

- A run's file is `<runs dir>/<run_id>.morpheus`; the runs folder is created at startup when missing.
- An opened run stays in memory until the server stops. A request to a run not opened in this process answers 409.

### One worker per run (`server::RunWorker`)

- Each run has one thread that owns its `turn::HostedSession`, its own `llamaserver::LlamaServerBackend` (wrapped by `SlotBackend`, below) and a queue of jobs. The backend is not thread-safe; several backends on one llama-server is its supported use.
- HTTP handler threads put a job on the queue and wait for it. A turn's job reports its events (action, tokens, sentences, done or error) to the handler through a thread-safe channel, and the handler writes them to the stream. The job carries a stop source that the handler triggers when the client disconnects.
- Jobs run one at a time, in arrival order.
- When the queue is empty, the worker runs `HostedSession::Maintain` with a stop token and a yield token. Only a turn stops it, at once, when the turn is queued (`RunWorker::Post` with `Preempt::Fold`), even behind other queued jobs. Every queued job asks it to yield: `Maintain` returns after its current step (an ended conversation's mark, one NPC's catch-up in one conversation, one pending share), so every other job (`events`, `participants`, `conversations`, `end`, `memories/share`, GET, a reopen) waits for one step, not for the whole pass (one catch-up per NPC who is behind, per conversation). A step in progress finishes, and a pass yields only after a step of its own, so a host that polls or records events often cannot keep folding from making progress. `Maintain` resumes from its marks when the queue is empty again. `Maintain` returning true with nothing left to fold leaves the worker idle until the next job.

### Slots

- `server::SlotPool` is shared by every run. At startup the server reads the chat server's slot count (`LlamaServerBackend::SlotCount`, from `/props` `total_slots`).
- Slot 0 is the upkeep slot: every call made from `Maintain` (folding, condensing). The other slots go to conversations, keyed by run and conversation id, least recently used first. With more open conversations than slots, the conversation used least recently loses its slot; its next turn takes one again and rebuilds its prompt.
- Ending a conversation frees its slot.
- With one slot, upkeep and turns share slot 0; the server logs a warning at startup.
- `server::SlotBackend` is an `inference::LlmBackend` that forwards to the run's backend and sets `GenerationSpec::slot` to the worker's current slot when the call left it at -1. The worker sets the current slot before each job (the conversation's) and before `Maintain` (0). `HostedSession` is unchanged.

### Shutdown

Ctrl+C (SIGINT) or SIGTERM: the server stops accepting connections, stops running turns (nothing committed; a stream already started ends with `error` 503, "the server is stopping") and running folds through their stop tokens, joins the workers, and closes the stores.

### Settings and flags

- The server reads `settings.json` (`--settings`, default `./settings.json`), the same file as the app. Its backend must be `llama_server`; any other backend exits at startup with a message. It supplies the chat and embedding URLs, the budgets and `context_size`.
- Flags: `--port` (default 4998), `--runs` (default `./runs`), `--settings`.
- Startup log: the port, the runs folder, both servers' model ids and the slot count.

## 3. Engine changes

### Event overflow in `turn::HostedSession`

- The tail holds the observation, the events recorded since the conversation's last turn, the request's events, and the player text. When they do not fit `tail_allowance`, the oldest events are left out of the prompt until the rest fits. The observation, the player text and the instruction must fit on their own; when they do not, `RunTurn` throws `TurnError` before the resolve call, as in D1.
- `core::HostedTurn` gains `int eventsOmitted = 0`: how many of the events before the turn its prompt left out. The store keeps every event.
- A turn's L3 transcript entry (`Entries`) shows the same events its prompt showed, so one long burst of events does not overflow L3 later.
- Store: `hosted_turns` gains `events_omitted INTEGER NOT NULL DEFAULT 0`, so `RunStore`'s schema goes to version 4. A version 3 file is upgraded in place in a transaction (`ALTER TABLE`; existing turns read as 0); a version 2 file gains the hosted tables, already with the new column, in one step. Owned runs are unaffected.
- D1's documented overflow behaviour (`RecordEvents` and `RunTurn` comments, the hosted-world spec's section 6) is replaced by this one.

### `llamaserver::LlamaServerBackend::SlotCount()`

The chat server's `total_slots` from the `/props` the constructor already reads. A missing or non-positive value is an `InferenceError` naming the URL.

## 4. Components

New module `src/server`, library `morpheus_server_lib` (namespace `morpheus::server`). It depends on `turn` and `llamaserver`, and only it and `tools/server` depend on it. It is not one of the engine libraries a vendored build exposes.

| File | Responsibility |
|---|---|
| `SlotPool.h/.cpp` | Slot assignment: upkeep slot 0, conversations least recently used first, freeing on end. Thread-safe. |
| `SlotBackend.h/.cpp` | The `LlmBackend` wrapper that sets the current slot on calls left at -1. |
| `SentenceSplitter.h/.cpp` | Pieces in, finished sentences out (section 1). |
| `RunWorker.h/.cpp` | One run: thread, job queue, `HostedSession`, backend, idle `Maintain`, stop and join. |
| `RunRegistry.h/.cpp` | `run_id` to worker; the run id rule and file path; `open` (create the worker or reopen); shutdown of every worker. Takes a backend factory, so tests supply fakes. |
| `Api.h/.cpp` | Transport-free: a method, a path and a body in; a status and a JSON body, or a stream of events, out. Every route, validation and status mapping lives here. |
| `HttpServer.h/.cpp` | cpp-httplib: routes to `Api`, SSE writing, stop on disconnect, `api_version`, the exclusive port, start and stop. The only file of the module that includes `<httplib.h>`. |

`tools/server/main.cpp` builds `morpheus_server`: flags, settings, the `LlamaServerBackend` factory, signal handling, the startup log, run until a signal.

### Build

- `MORPHEUS_BUILD_SERVER`, default `PROJECT_IS_TOP_LEVEL` (R1). It needs `MORPHEUS_BUILD_LLAMA_SERVER` (configure fails otherwise) and never links llama.cpp.
- Built in every top-level preset: `tests`, `asan`, `dev`, `dev-asan`, `linux-tests`, `linux-release`.
- A vendored build (not top level) builds no server; the consumer check still finds only the engine libraries and `morpheus_httplib`.

## Tests (written first, Catch2)

### Fast suite, no network

- `SlotPool`: upkeep slot 0; conversations least recently used first; more conversations than slots; a freed slot is reused; one slot only.
- `SlotBackend`: sets the slot on calls left at -1, keeps an explicit one, forwards everything else.
- `SentenceSplitter`: each ending; closing quotes and brackets; a piece split inside a word or before the punctuation; `3.5`; `...`; the remainder at the end; abbreviations split (documented).
- `Api`, with a backend factory of scripted backends:
  - every status of section 1, with the field named for 400s;
  - the run id rule; `open` twice with the same and with changed data;
  - a turn's events in order (`action` before any `token`, each `sentence` after its last token, `done` last) and `done`'s fields;
  - a refused action;
  - `api_version` on every response.
- `RunWorker`:
  - jobs in arrival order;
  - idle `Maintain` stops when a turn is queued (also behind a waiting job) and resumes after it; other jobs wait for its pass;
  - an ended conversation is folded in the background;
  - shutdown stops a running turn, which commits nothing.
- `HostedSession` event overflow:
  - newest events kept;
  - `eventsOmitted` stored and read back;
  - the L3 entry shows the same events;
  - the observation alone over the tail is still `TurnError`.
- `RunStore`: a version 3 file upgrades to version 4 with its turns at 0 omitted; a version 2 file upgrades to version 4 in one step.

### End to end, real sockets, `FakeLlamaServer` behind

- D1's fixtures (`open.json`, `conversation.json`, `radiant.json`, `invalid_requests.json`) replayed over HTTP: statuses, the SSE stream, and the stored state through `GET`. These are the contract tests the brief's section 6 asks for.
- A client that disconnects mid-line: no turn committed, and the run's next turn works.
- Two runs at once: each conversation's `/completion` calls carry its own `id_slot`, and folding uses slot 0.
- A port already in use: the server refuses to start.

### Live, `[server]`

The `conversation` fixture end to end over HTTP against real llama-servers. The test prints the time from the request to the first `sentence` event with a warm cache. R4's 1.5 s target is for the host's RTX 3060 Ti, so it is measured and recorded in the README, not asserted.

## Out of scope

- R8's `"decide"`, the optional review per turn, and an explicit call tag: later sub-projects.
- Per-speaker memory (E) and the host's prompt pack (F).
- Concurrent turns within one run.
- Authentication or binding beyond 127.0.0.1.
- Closing an idle run before the server stops.

## Documentation

- README: a "morpheus_server" section: running it against llama-server, the flags, the endpoints, an example session with curl, the measured first-sentence time.
- `CHANGELOG.md` (new, at the repo root): API 1.0.0. API 1.1.0 (E1, `2026-11-16-per-speaker-memory-design.md`) adds the two memory routes and changes behaviour: each NPC remembers only what it witnessed.
- CLAUDE.md: `src/server` and `tools/server` in the layout; `src/server` among the modules that include `<httplib.h>`; the `MORPHEUS_BUILD_SERVER` option.
- Architecture spec: the server module row and its dependencies.
- The hosted-world spec: section 6's overflow paragraph points here.
