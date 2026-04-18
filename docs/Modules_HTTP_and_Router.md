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
| POST | `/initialize` | **`director` `/initialize`** first (same **`InitializeModuleRequest`** body), then **`session_store` `/initialize`**. Returns the session_store response body (or the director error if that step fails). |
| POST | `/turn` | **Director** `/message` → **`session_store` `/persist_turn`** (sequencing inside persist) → returns Director JSON body. |
| POST | `/proxy` | **Allowlisted** forward to another module (see below). |

### `/proxy` contract

Request body: **`ModuleProxyRequest`** (`sourceModule`, `targetModule`, `targetPath`, `method`, optional `body`).

- **`targetModule`** may be a logical alias (e.g. `generic_llm_provider`); resolved via `EngineConfiguration.ResolveProxyTargetModuleKey`.
- **`targetPath` + method** must match an entry in that module’s `endpoints[]` in `engine_config.json`, or the router returns **403**.
- Proxied responses must be **`Content-Type: application/json`** or the router fails loud (no silent coercion).

### `/turn` orchestration (current behavior)

1. Deserialize **`TurnRequest`**.
2. **`director` `/message`** with **`DirectorMessageRequest`** (`turn`, `playerInput` only; run and project are enforced at **`session_store`** and **`/initialize`**).
3. Expect **`IntentResponse`**-compatible JSON (`ok`, `intent`, `params`) where `intent` is **`narration`** and narration lives in **`params.text`**.
4. **`session_store` `/persist_turn`** with **`TurnPersistRequest`** (`turn`, `playerInput`, `intentResponseBody`; run comes from that module’s last successful **`/initialize`**) — sequencing and `turn >= 1` enforced here.
5. Return the Director response body to the client unchanged on full success.

**Note:** `intent_extractor` `/intent` is not on this path anymore; the module may still run for experiments or future routing.

## Session store (`MorpheusEngine.SessionStoreModule`)

Host: `SessionStoreHost.cs`; persistence: `RunPersistence.cs`.

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Create run directory + SQLite + schema + turn-0 snapshot; optional lore seed from CSV. |
| POST | `/persist_turn` | Insert `events`, append `snapshots` for the **bound** run (last successful **`/initialize`** on this process; transactional; enforces next turn = `MAX(snapshots.turn) + 1` and `turn >= 1`). |

## Director (`MorpheusEngine.Director`)

| Method | Path | Role |
|--------|------|------|
| POST | `/initialize` | Accept **`InitializeModuleRequest`** (`gameProjectId`, `runId`); load **`system/instructions.md`** + lore CSV once; bind that single run in memory. Second call in the same process → **409**. |
| POST | `/message` | Accept **`DirectorMessageRequest`** (`turn`, `playerInput`); requires prior **`/initialize`** in this process; call LLM via **`router /proxy`** → **`generic_llm_provider` `/chat`**; return **`IntentResponse`** narration shim. |

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

`ollama_port` on the `llm_provider_qwen` module row in `engine_config.json` configures the Ollama base URL port.

## Where to add a new module

1. Add **`port_key`** to `EnginePortMap.RequiredPortKeys` and **`EnsureRequiredModulesPresent`** in `EngineConfigLoader.cs` (if the module is mandatory).
2. Add **`ports`** entry and a full **`modules[]`** block with **`endpoints`** (every route you want `/proxy` or humans to hit).
3. Add **`MorpheusEngine.*.csproj`** and register it in **`dotnet/MorpheusEngine.sln`**.
4. Implement **`/health`** and **`/shutdown`** so `ManagedModule` lifecycle works.
5. If the router should orchestrate it, extend **`Router.cs`**; if only proxied, callers use **`/proxy`** with the new path registered in JSON.
