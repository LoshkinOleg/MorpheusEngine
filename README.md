# Morpheus Engine

Local-first narrative game runtime using modular .NET services coordinated over localhost HTTP.

## What this repo is

- `.NET 9` solution at `dotnet/MorpheusEngine.sln`
- WPF operator UI in `dotnet/src/MorpheusEngine.App`
- Multi-process engine host in `dotnet/src/MorpheusEngine.Core`
- Router-driven turn orchestration in `dotnet/src/MorpheusEngine.RouterModule`
- SQLite-backed run persistence in `dotnet/src/MorpheusEngine.SessionStoreModule`
- LLM provider and embeddings providers backed by local Ollama
- Data-driven project content under `game_projects/<gameProjectId>/`

## High-level runtime model

Each backend module is a standalone executable binding `HttpListener` on `127.0.0.1:<port>` as configured in `engine_config.json`.

The WPF app talks to Router (`/initialize`, `/turn`). Router then orchestrates module calls through:

- manifest-selected `turn_pipeline` (from `game_projects/<id>/manifest.json`)
- endpoint allowlists declared per module in `engine_config.json`
- module aliases (`generic_*`) resolved at runtime

Cross-module calls should go through Router `/proxy`, not direct module-to-module client calls from UI code.

## Repository layout

- `dotnet/src/MorpheusEngine.App` - WPF UI, HTTP test tab, game flow UI
- `dotnet/src/MorpheusEngine.Core` - config loading, shared contracts, process lifecycle
- `dotnet/src/MorpheusEngine.RouterModule` - `/initialize`, `/turn`, `/proxy` orchestrator
- `dotnet/src/MorpheusEngine.SessionStoreModule` - run DB creation, turn persistence, memory endpoints
- `dotnet/src/MorpheusEngine.MemoryDirector` - memory-managed director (default via alias)
- `dotnet/src/MorpheusEngine.Director` - legacy/simple director implementation
- `dotnet/src/MorpheusEngine.IntentExtractor` - intent extraction endpoint for experiments/future routing
- `dotnet/src/MorpheusEngine.LlmProvider_qwen` - Ollama chat/generate/token_count bridge
- `dotnet/src/MorpheusEngine.Embeddings_ollama` - Ollama embeddings bridge (`embeddings_num_ctx` in config sets `options.num_ctx` per request, e.g. 2048 for `nomic-embed-text`)
- `game_projects/sandcrawler` - sample game project
- `engine_config.json` - source of truth for modules, ports, aliases, pipelines
- `third_party/ollama` - expected local Ollama binaries/models location

## Quick start (Windows)

Prerequisites
   - .NET SDK 9
   - Local Ollama assets configured per this repo (`third_party/ollama`, model files)

## Testing

The repo now has an automated .NET test harness under `dotnet/tests/`:

- `dotnet/tests/MorpheusEngine.Tests.Unit`
- `dotnet/tests/MorpheusEngine.Tests.Integration`

Run everything:

```powershell
dotnet test dotnet/MorpheusEngine.sln
```

Run only unit tests:

```powershell
dotnet test dotnet/tests/MorpheusEngine.Tests.Unit/MorpheusEngine.Tests.Unit.csproj --filter "Category=Unit"
```

Run only integration tests:

```powershell
dotnet test dotnet/tests/MorpheusEngine.Tests.Integration/MorpheusEngine.Tests.Integration.csproj --filter "Category=Integration"
```

For the full harness guide, shared fixtures, and test-authoring rules, see `docs/Testing_Harness.md`.

## Configuration and contracts

Primary configuration file: `engine_config.json`.

Top-level keys:

- `ports` - `port_key -> TCP port` map
- `modules` - process definitions with launch path and endpoint allowlist
- `module_aliases` - logical alias -> concrete module key
- `turn_pipelines` - reusable `/turn` orchestration definitions

Current alias defaults:

- `generic_director` -> `memory_director`
- `generic_llm_provider` -> `llm_provider_qwen`
- `generic_embeddings` -> `embeddings_ollama`

## Router and module HTTP surface

### Common module endpoints

Most modules implement:

- `GET /info`
- `GET /health`
- `POST /initialize`
- `POST /shutdown` (where applicable)

Health lifecycle is explicit via `ModuleHealthResponse.initialized`:

- listening but not initialized: `200` + `initialized: false`
- initializing: typically `503` + `initialized: false`
- ready: `200` + `initialized: true`

### Router endpoints

- `GET /info`
- `GET /health`
- `POST /shutdown`
- `POST /initialize`
- `POST /turn`
- `POST /proxy`

`/proxy` rules:

- target module can be alias or concrete key
- `(path, method)` must be allowlisted on target module
- proxied response must be JSON (`application/json`)

### Turn orchestration

Router executes the selected pipeline, not a hardcoded flow. Built-in presets:

- `memory_director_default`
- `simple_director_default`

Default shape:

1. `generic_director` `POST /message`
2. `session_store` `POST /persist_turn`
3. map director output -> `TurnResponse`

## Game projects and persistence

Project layout:

```text
game_projects/<gameProjectId>/
  manifest.json
  lore/default_lore_entries.csv
  system/instructions.md
  saved/<runId>/world_state.db
```

`gameProjectId` is validated as a single path segment (no slashes, no `..`).

Manifest controls:

- `id` and `title`
- `turn_pipeline` (defaults to `memory_director_default` if omitted)
- `required_modules` (project-specific required module keys/aliases)

SQLite (`world_state.db`) core tables include:

- `meta`
- `events`
- `snapshots`
- `lore`
- `turn_execution` (reserved)
- `pipeline_events` (used for diagnostics, including MemoryDirector telemetry)

Session store behavior:

- `/initialize` creates DB/schema and inserts turn-0 snapshot
- `/persist_turn` is transactional and enforces turn monotonicity
- persisted turn payload includes `playerInput` and `directorResponseBody`

## Memory system status

The active direction is a memory-managed director model:

- `memory_director` handles main narration turns behind `generic_director`
- `session_store` exposes dedicated `/memory/*` endpoints for context load/persist/search
- embeddings are intentionally separated in `embeddings_ollama` (not coupled into chat provider)
- budget diagnostics are emitted as `pipeline_events` (`eventType: "memory_context_budget"`)

This follows the long-term intent documented in the memGPT planning notes: durable state, searchable recall, archival memory, and tool-mediated agent behavior while preserving process and allowlist boundaries.

## Extending the engine safely

To add a new module:

1. Add module row in `engine_config.json` with `port_key`, launch artifact, and `endpoints`.
2. Add required ports/validation paths in `EngineConfigLoader` if the module is engine-mandatory.
3. Add project to `dotnet/MorpheusEngine.sln`.
4. Implement lifecycle endpoints (`/health`, `/shutdown`) to integrate with `ManagedModule`.
5. Update/create `turn_pipelines` if Router should orchestrate it.

Design guidance synthesized from architecture audit:

- keep pipeline and module roles data-driven through config/manifest
- avoid hardcoding module names where aliases can represent abstraction points
- preserve fail-fast behavior on invalid config/contracts
- keep storage ownership in `session_store` (no direct multi-process DB writes by other modules)

## Example turn request

```json
{
  "runId": "uuid",
  "gameProjectId": "sandcrawler",
  "turn": 1,
  "playerInput": "I order the crew to seal the hull breach."
}
```
