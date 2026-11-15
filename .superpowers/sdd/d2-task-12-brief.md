### Task 12: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`
- Create: `CHANGELOG.md`
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`

**Interfaces:**
- Consumes: the finished server (Tasks 1-11).
- Produces: documentation only.

- [ ] **Step 1: README: a "morpheus_server" section**

Add a section `## morpheus_server (hosted runs over HTTP)` after `## Play in the terminal`, with:

- What it is, in two sentences: a localhost HTTP server through which a host (the Skyrim/Mantella fork) plays hosted runs; design in `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, hosted runs in `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, the host data and request format in `tests/hosted/fixtures/README.md`.
- Build: it builds in every top-level preset (`MORPHEUS_BUILD_SERVER`, default on when top level), with no llama.cpp; the binary is `build/<preset>/bin/<config>/morpheus_server(.exe)`.
- Run: start the two llama-servers as in "Models through llama-server" (with `--parallel 3` or more: slot 0 is for folding, the others for conversations), set `"backend": "llama_server"` in settings.json (its `server.slot` is not used by the server, which hands out slots itself), then:

````markdown
```
./build/dev/bin/Release/morpheus_server --port 4998 --runs runs --settings settings.json
```
````

- The flags and defaults (`--port 4998`, `--runs ./runs`, `--settings ./settings.json`), that it listens on 127.0.0.1 only, that a run's file is `<runs>/<run_id>.morpheus`, and that Ctrl+C stops it (running turns commit nothing).
- The endpoint table from the spec's section 1 and the turn stream's events, in short.
- An example session with curl (open with `tests/hosted/fixtures/open.json`, start a conversation, record an event, play a turn with `curl -N`, end the conversation), with the stream's shape shown as text:

````markdown
```
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/open -H "Content-Type: application/json" --data @tests/hosted/fixtures/open.json
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations -H "Content-Type: application/json" \
  --data '{"participants": ["0001A6A4", "00000007"], "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
curl -N -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations/c1/turn -H "Content-Type: application/json" \
  --data '{"speaker": "0001A6A4", "player_text": "Can you follow me?", "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
```

```
event: action
data: {"action":{"id":"follow","params":{}},"api_version":"1.0.0","speaker":"0001A6A4"}

event: token
data: {"text":"Lead"}
...
event: sentence
data: {"text":"Lead the way, then."}

event: done
data: {"action":{"id":"follow","params":{}},"events_omitted":0,"line":"Lead the way, then.","speaker":"0001A6A4","timings":{...},"turn":1}
```
````

- The errors table (400, 404, 409, 422, 503, 500) in one short list.
- The live test: `"[server]"` now also plays the conversation fixture over HTTP and prints each turn's time to its first sentence. Write no figure: the controller adds the measured one after running the test against the owner's servers.

In `## Layout`, add:

```markdown
- `src/server`: `morpheus_server`'s library: run workers, slots, the transport-free API and the HTTP adapter (cpp-httplib; every top-level preset, `MORPHEUS_BUILD_SERVER`)
- `tools/server`: `morpheus_server`, the HTTP server for hosted runs (every top-level preset)
```

and in `### The [server] tests`, add one sentence: the server's own `[server]` test plays the conversation fixture over HTTP against your servers and prints each turn's time to its first sentence.

- [ ] **Step 2: CHANGELOG.md**

Create `CHANGELOG.md` at the repository root:

```markdown
# Changelog

The morpheus_server API's versions (`api_version` on every response). A breaking change bumps the major version and
gets an entry here with a migration note.

## API 1.0.0

The first version (design: `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`).

- `GET /health`; `POST /v1/runs/{run_id}/open`; `POST /v1/runs/{run_id}/conversations`;
  `POST /v1/runs/{run_id}/conversations/{cid}/turn` (Server-Sent Events: `action`, `token`, `sentence`, then `done` or
  `error`), `/events`, `/participants`, `/end`; `GET /v1/runs/{run_id}/conversations/{cid}`; `POST /v1/tokenize`;
  `POST /v1/embeddings`.
- Errors: 400 (a malformed request, naming the field), 404, 409 (a run not opened in this process), 422 (the
  observation and player text do not fit the tail), 503 (a llama-server unreachable), 500.
- `speaker: "decide"` is refused with 400 until a later version.
```

- [ ] **Step 3: CLAUDE.md**

- In "Build and test", after the `MORPHEUS_GPU_BACKEND` line, add: `` - `MORPHEUS_BUILD_SERVER` (default on when top level) builds `morpheus_server` and its library `morpheus_server_lib` in every preset, without llama.cpp; it needs `MORPHEUS_BUILD_LLAMA_SERVER`. ``
- Change the "Real-server tests" line's end to mention that `[server]` also covers `morpheus_server` over HTTP, and add: `` - The server's own tests that need no real server are tagged `[hosted-server]`. ``
- In "Layout", change the `src/llamaserver` line's "Only it and the tests include <httplib.h>" to "Only it, src/server/HttpServer.cpp and the tests include <httplib.h>", and add after it:

```markdown
- src/server: `morpheus_server_lib`, the server for hosted runs: `SlotPool` and `SlotBackend` (a llama-server slot per conversation, slot 0 for folding), `RunWorker` (one thread per run owning its `HostedSession` and backend, folding when idle), `RunRegistry` (run ids and files), `Api` (every route and status, without sockets), `TurnStream`, `SentenceSplitter`, `HttpServer` (cpp-httplib, Server-Sent Events). Never links llama.cpp.
- tools/server: `morpheus_server` (`--port`, `--runs`, `--settings`), every top-level preset.
```

- [ ] **Step 4: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`'s module table, add a row after `turn`:

```markdown
| `server` | `morpheus_server` for hosted runs: a worker thread per run owning its `HostedSession` and backend, a llama-server slot per conversation (slot 0 for folding), a transport-free `Api` and an `HttpServer` on cpp-httplib streaming each turn as Server-Sent Events; the `morpheus_server` executable in `tools/server`. Behind `MORPHEUS_BUILD_SERVER` (default on when top level); never links llama.cpp | turn, llamaserver |
```

In its `hosted_turns` table row, add `, events omitted` after `timings`. In "## Hosted runs", replace the sentence "Every event recorded since a conversation's last turn joins its next turn's tail; when they alone are over `tail_allowance`, every later turn of that conversation is a `TurnError` until it ends." with "Every event recorded since a conversation's last turn joins its next turn's tail; when the tail is over `tail_allowance`, the oldest events leave the prompt (and that turn's L3 entry) until it fits, the turn records how many (`eventsOmitted`, store version 4), and the store keeps them all." and replace "A slot per conversation and any concurrency across conversations are left to D2 (`morpheus_server`), which can add them without changing this API." with "`morpheus_server` (module `server`) keeps that contract: one worker thread per run, a slot per conversation."

- [ ] **Step 5: The hosted-world spec's section 6**

In `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, replace the bullet that starts "**Events recorded between turns that do not fit are a `turn::TurnError` too.**" with:

```markdown
- **Events recorded between turns that do not fit leave the prompt, oldest first.** This replaced D1's `TurnError` in sub-project D2 (`docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, section 3): the newest events that fit stay in the tail, the turn records how many it left out (`eventsOmitted`), its L3 entry leaves out the same ones, and the store keeps them all. The observation, the player text and the instruction must still fit on their own, or the turn is a `TurnError`.
```

and in its section 1, change "(section 6 for the overflow)" to "(section 6 for the overflow, as D2 changed it)".

- [ ] **Step 6: Check the docs**

Run: `LC_ALL=C grep -c $'[\x80-\xFF]' README.md CLAUDE.md CHANGELOG.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md`
Expected: 0 for each.

- [ ] **Step 7: Commit**

```bash
git add README.md CLAUDE.md CHANGELOG.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md
git commit -m "Document morpheus_server: README section, CHANGELOG for API 1.0.0, CLAUDE.md layout, the architecture and hosted-world specs"
```

---

## Final verification (controller)

- `ctest --preset tests` and `ctest --preset asan`: all pass; the `[server]` tests skip.
- `cmake --build --preset dev`: clean, including `morpheus_server`.
- The consumer check (README, "Using Morpheus from another project"): ok.
- With the owner's llama-servers running: `./build/tests/bin/Debug/morpheus_tests.exe "[server]"`, and the measured first-sentence figures go into the README.
- No `morpheus_tests`, `morpheus_server` or `ctest` process left running.
