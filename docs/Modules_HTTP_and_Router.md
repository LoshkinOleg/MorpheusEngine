# Modules, HTTP surface, and router

Last reviewed: 2026-05-08.

All backend modules are **standalone executables** that bind **`HttpListener`** to **`http://127.0.0.1:{port}/`** where `{port}` comes from `engine_config.json` → `ports.<port_key>`.

## Terminology: run vs session

- **Run** — persistent identity (`gameProjectId`, `runId`) used for filesystem/DB state.
- **Session (module process)** — runtime binding after `POST /initialize` in a given process; many modules are single-run-per-process.
- **Session (UI)** — human interaction lifecycle; independent from run identity.

## Common endpoints

Nearly every module implements:

- **`GET /info`** — JSON metadata (`ModuleInfoResponse`).
- **`GET /health`** — Lifecycle JSON (`ModuleHealthResponse`: `ok`, `status`, **`initialized`**). Before `POST /initialize` the host expects **HTTP 200** with `initialized: false` (`status` e.g. `awaiting_initialize`); while init runs, **503** with `initialized: false`; when ready, **HTTP 200** with `initialized: true`. `ManagedModule.WaitUntilListeningAsync` waits for the first case; `WaitUntilReadyAsync` waits until `initialized` is true.
- **`POST /shutdown`** — Graceful stop; stops the accept loop.

## Router (`MorpheusEngine.RouterModule`)

File: `dotnet/src/MorpheusEngine.RouterModule/Router.cs`.

| Method | Path | Behavior |
|--------|------|------------|
| GET | `/info`, `/health` | Router identity. |
| POST | `/shutdown` | Stops router listener. |
| POST | `/initialize` | Binds the router to a run, loads the game manifest, and validates the selected `turn_pipeline`. |
| POST | `/turn` | Executes the manifest-selected `turn_pipeline` through the same allowlisted forwarding path used by `/proxy`, then returns a router-owned `TurnResponse`. |
| POST | `/proxy` | **Allowlisted** forward to another module (see below). |

### `/proxy` contract

Request body: **`ModuleProxyRequest`** (`sourceModule`, `targetModule`, `targetPath`, `method`, optional `body`).

- **`targetModule`** may be a logical alias (e.g. `generic_llm_provider`); resolved via `EngineConfiguration.ResolveProxyTargetModuleKey`.
- **`targetPath` + method** must match an entry in that module’s `endpoints[]` in `engine_config.json`, or the router returns **403**.
- Proxied responses must be **`Content-Type: application/json`** or the router fails loud (no silent coercion).

### `/turn` pipeline orchestration

`engine_config.json` defines reusable `turn_pipelines`, and each game manifest selects one with `turn_pipeline`.

The built-in presets are:

- **`memory_director_default`** — `generic_director POST /message`, then `session_store POST /persist_turn`, with `TurnResponse.text` mapped from `DirectorMessageResponse.text`. In the default config, `generic_director` resolves to `memory_director`.
- **`simple_director_default`** — the same two-step shape, intended for configs where `generic_director` resolves to the legacy `director`.

Each pipeline step has a constrained `body_template`. Supported placeholders are `{{turn}}`, `{{playerInputJson}}`, `{{previous.rawBody}}`, `{{previous.rawBodyJson}}`, `{{step.<id>.rawBody}}`, and `{{step.<id>.rawBodyJson}}`. The router does not evaluate arbitrary expressions.

If the director step succeeds but `session_store /persist_turn` fails, the router returns the persistence failure and logs that module state may be ahead of SQLite. A pipeline can omit the persistence step, but there is no hidden router-level persistence toggle.

**Note:** `intent_extractor` `/intent` is not on this path anymore; the module may still run for experiments or future routing.

## Session store (`MorpheusEngine.SessionStoreModule`)

Host: `SessionStoreHost.cs`; persistence: `RunPersistence.cs`.

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Create run directory + SQLite + schema + turn-0 snapshot; optional lore seed from CSV. |
| POST | `/persist_turn` | Insert `events`, append `snapshots` for the **bound** run (last successful **`/initialize`** on this `session_store` process; transactional; enforces next turn = `MAX(snapshots.turn) + 1` and `turn >= 1`). |
| POST | `/memory/pipeline_events/recent` | Return recent diagnostic pipeline events such as MemoryDirector context-budget telemetry. These events are stored outside `agent_messages`, so they do not affect recall FTS. |

## Director (`MorpheusEngine.Director`)

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Accept **`InitializeModuleRequest`** (`gameProjectId`, `runId`); load **`system/instructions.md`** + lore CSV once; bind that single run in memory. Second call in the same process → **409**. |
| POST | `/message` | Accept **`DirectorMessageRequest`** (`turn`, `playerInput`); requires prior **`/initialize`** in this process; call LLM via **`router /proxy`** → **`generic_llm_provider` `/chat`**; return **`DirectorMessageResponse`**. |

State is **in-process memory** for **one run per Director process** (lost if Director restarts). Lore and GM instructions are read at **`/initialize`**, not lazily on first **`/message`**.

## Intent extractor (`MorpheusEngine.IntentExtractor`)

| Method | Path | Role |
|--------|------|------|
| POST | `/intent` | **`IntentRequest`** → LLM via proxy `/generate` → strict **`IntentResponse`** JSON catalog. |

## LLM provider Qwen (`MorpheusEngine.LlmProvider_qwen`)

| Method | Path | Upstream |
|--------|------|----------|
| POST | `/generate` | Ollama **`/api/generate`** (`LlmGenerateRequest`: prompt + optional system; Ollama model from **`llm_provider_qwen.default_chat_model`**). |
| POST | `/chat` | Ollama **`/api/chat`** (`ChatGenerateRequest`: `messages[]` only; Ollama model from **`llm_provider_qwen.default_chat_model`** in `engine_config.json`). |
| POST | `/token_count` | Token-count probe for the configured model. Uses Ollama prompt stats when available and returns `exact: true`; otherwise returns a deterministic estimate with `exact: false`. |

`ollama_port` on the `llm_provider_qwen` module row in `engine_config.json` configures the Ollama base URL port.

## MemoryDirector budget telemetry

MemoryDirector records per-step context compiler diagnostics in `session_store.pipeline_events` with payload discriminator `eventType: "memory_context_budget"`.

The telemetry describes the compiled context, not the raw `/memory/load_context` response. It includes `numCtx`, the 70% target token budget, estimated characters, provider/heuristic token counts, and itemized `included`, `truncated`, or `omitted` sections with reasons. Token counts with `exact: false` are estimates and should be used for debugging, not as tokenizer ground truth.

## Where to add a new module

1. Add **`port_key`** to `EnginePortMap.RequiredPortKeys` and **`EnsureRequiredModulesPresent`** in `EngineConfigLoader.cs` (if the module is mandatory).
2. Add **`ports`** entry and a full **`modules[]`** block with **`endpoints`** (every route you want `/proxy` or humans to hit).
3. Add **`MorpheusEngine.*.csproj`** and register it in **`dotnet/MorpheusEngine.sln`**.
4. Implement **`/health`** and **`/shutdown`** so `ManagedModule` lifecycle works.
5. If the router should orchestrate it on `/turn`, add or update a **`turn_pipelines`** preset; if only proxied, callers use **`/proxy`** with the new path registered in JSON.
