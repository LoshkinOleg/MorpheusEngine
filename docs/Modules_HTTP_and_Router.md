# Modules, HTTP surface, and router

All backend modules are **standalone executables** that bind **`HttpListener`** to **`http://127.0.0.1:{port}/`** where `{port}` comes from `engine_config.json` → `ports.<port_key>`.

## Common endpoints

Nearly every module implements:

- **`GET /info`** — JSON metadata (`ModuleInfoResponse`).
- **`GET /health`** — Liveness (`ModuleHealthResponse`); used by `ManagedModule.WaitUntilReadyAsync`.
- **`POST /shutdown`** — Graceful stop; stops the accept loop.

## Router (`MorpheusEngine.RouterModule`)

File: `dotnet/src/MorpheusEngine.RouterModule/Router.cs`.

| Method | Path | Behavior |
|--------|------|------------|
| GET | `/info`, `/health` | Router identity. |
| POST | `/shutdown` | Stops router listener. |
| POST | `/initialize` | **`director` `/initialize`** first (same **`RunStartRequest`** body), then **`session_store` `/initialize`**. Returns the session_store response body (or the director error if that step fails). |
| POST | `/turn` | Validates turn → **Director** `/message` → persists → returns Director JSON body. |
| POST | `/proxy` | **Allowlisted** forward to another module (see below). |

### `/proxy` contract

Request body: **`ModuleProxyRequest`** (`source_module`, `target_module`, `target_path`, `method`, optional `body`).

- **`target_module`** may be a logical alias (e.g. `generic_llm_provider`); resolved via `EngineConfiguration.ResolveProxyTargetModuleKey`.
- **`target_path` + method** must match an entry in that module’s `endpoints[]` in `engine_config.json`, or the router returns **403**.
- Proxied responses must be **`Content-Type: application/json`** or the router fails loud (no silent coercion).

### `/turn` orchestration (current behavior)

1. Deserialize **`TurnRequest`**.
2. **`session_store` `/validate_turn`** with **`TurnValidateRequest`**.
3. On success, **`director` `/message`** with **`DirectorMessageRequest`** (same field names as `TurnRequest`).
4. Expect **`IntentResponse`**-compatible JSON (`ok`, `intent`, `params`) where `intent` is **`narration`** and narration lives in **`params.text`**.
5. **`session_store` `/persist_turn`** with **`TurnPersistRequest`** including `intentResponseBody` string.
6. Return the Director response body to the client unchanged on full success.

**Note:** `intent_extractor` `/intent` is not on this path anymore; the module may still run for experiments or future routing.

## Session store (`MorpheusEngine.SessionStoreModule`)

Host: `SessionStoreHost.cs`; persistence: `RunPersistence.cs`.

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Create run directory + SQLite + schema + turn-0 snapshot; optional lore seed from CSV. |
| POST | `/validate_turn` | Compare client `turn` to `MAX(snapshots.turn) + 1`. |
| POST | `/persist_turn` | Insert `events`, append `snapshots` for the turn (transactional). |

## Director (`MorpheusEngine.Director`)

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Accept **`RunStartRequest`** (`gameProjectId`, `runId`); load **`system/instructions.md`** + lore CSV once; bind that single run in memory. Second call in the same process → **409**. |
| POST | `/message` | Accept **`DirectorMessageRequest`**; requires matching **`runId`** / **`gameProjectId`** after **`/initialize`**; call LLM via **`router /proxy`** → **`generic_llm_provider` `/chat`**; return **`IntentResponse`** narration shim. |

State is **in-process memory** for **one run per Director process** (lost if Director restarts). Lore and GM instructions are read at **`/initialize`**, not lazily on first **`/message`**.

## Intent extractor (`MorpheusEngine.IntentExtractor`)

| Method | Path | Role |
|--------|------|------|
| POST | `/intent` | **`IntentRequest`** → LLM via proxy `/generate` → strict **`IntentResponse`** JSON catalog. |

## LLM provider Qwen (`MorpheusEngine.LlmProvider_qwen`)

| Method | Path | Upstream |
|--------|------|----------|
| POST | `/generate` | Ollama **`/api/generate`** (`LlmGenerateRequest`: prompt + system + model). |
| POST | `/chat` | Ollama **`/api/chat`** (`ChatGenerateRequest`: model + messages[]). |

`ollama_port` on the `llm_provider_qwen` module row in `engine_config.json` configures the Ollama base URL port.

## Where to add a new module

1. Add **`port_key`** to `EnginePortMap.RequiredPortKeys` and **`EnsureRequiredModulesPresent`** in `EngineConfigLoader.cs` (if the module is mandatory).
2. Add **`ports`** entry and a full **`modules[]`** block with **`endpoints`** (every route you want `/proxy` or humans to hit).
3. Add **`MorpheusEngine.*.csproj`** and register it in **`dotnet/MorpheusEngine.sln`**.
4. Implement **`/health`** and **`/shutdown`** so `ManagedModule` lifecycle works.
5. If the router should orchestrate it, extend **`Router.cs`**; if only proxied, callers use **`/proxy`** with the new path registered in JSON.
