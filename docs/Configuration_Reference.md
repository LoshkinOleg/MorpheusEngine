# Configuration reference

## `engine_config.json` (repository root)

Loaded by **`EngineConfigLoader.GetConfiguration()`**. The loader walks upward from the executable / current directory until it finds **`engine_config.json`**.

## Bundled Ollama (`third_party/ollama`)

The **`llm_provider_qwen`** module expects a **bundled** Ollama install under the repository root (same directory that contains **`engine_config.json`**). The engine does not download these files for you.

1. **`third_party/ollama/`** — Put the contents of **`ollama-windows-amd64.zip`** from the [Ollama GitHub releases](https://github.com/ollama/ollama/releases) here so that **`ollama.exe`** exists at **`third_party/ollama/ollama.exe`** (extract the archive so the Windows binaries sit directly under `third_party/ollama/`, not nested in an extra folder unless your layout still resolves to that path).

2. **`third_party/ollama/models/`** — Put the contents of the **`models`** folder from the **Ollama tray app** install here (the same blobs Ollama uses when you pull models from the UI). The provider sets **`OLLAMA_MODELS`** to this directory for the child process.

Without this layout, **`LlmProvider_qwen`** fails at startup when it cannot find the bundled executable or model files.

### Top-level keys

| Key | Purpose |
|-----|---------|
| **`ports`** | Map of **`port_key` → TCP port** (int). Keys must match the known set in code (`EnginePortMap.RequiredPortKeys`) — no extras, no omissions. |
| **`modules`** | Array of process definitions: **`port_key`**, **`display_name`**, **`required`**, **`launch`**, **`endpoints`**, plus optional module-specific fields. |
| **`module_aliases`** | Optional. Maps logical names to real **`port_key`** values (e.g. `generic_llm_provider` → `llm_provider_qwen`). |
| **`turn_pipelines`** | Reusable `/turn` orchestration presets keyed by pipeline id. Game manifests select one with `turn_pipeline`. |

### `modules[]` row shape

- **`port_key`**: Stable id; must have a matching **`ports.<port_key>`** entry.
- **`required`**: If true, engine startup waits for **`GET /health`** on that module.
- **`launch.artifact`**: Path to built `.exe` or `.dll` (relative to repo root unless absolute).
- **`endpoints[]`**: Each **`path`**, **`method`** (`GET` or `POST`), optional **`description`**, **`request_contract`**, **`body_template`**.

**`request_contract`** ties into **`EngineContractExamples.TryGetRequestBodyTemplate`** in `EngineHttpContracts.cs` for UI samples / tooling.

### `turn_pipelines`

Each pipeline contains ordered `steps[]` and one `response_mapping`.

- **`steps[].target_module`** may be a concrete module key or a configured alias such as `generic_director`.
- **`steps[].path`** + **`steps[].method`** must match an endpoint allowlisted on the resolved module row.
- **`steps[].body_template`** supports only the router's constrained placeholders: `{{turn}}`, `{{playerInputJson}}`, `{{previous.rawBody}}`, `{{previous.rawBodyJson}}`, `{{step.<id>.rawBody}}`, and `{{step.<id>.rawBodyJson}}`.
- **`response_mapping.type`** is currently `director_message_response`, which parses `DirectorMessageResponse.text` and returns `TurnResponse(ok: true, text)`.

The default config defines **`memory_director_default`** and **`simple_director_default`**. Both preserve the existing two-step flow: `generic_director POST /message` followed by `session_store POST /persist_turn`.

### Module-specific optional fields

- **`module_aliases.generic_llm_provider`** must point at a concrete **`port_key`** (typically **`llm_provider_qwen`**). The loader merges **`generic_llm_provider`** JSON options onto that row.
- **That resolved row** must carry **`GenericLlmProviderOptions`** (**`num_ctx`** from the generic provider JSON). When the concrete key is **`llm_provider_qwen`**, the same row must also have **`QwenOptions`** (**`ollama_port`**, **`default_chat_model`**).

## `EngineConfiguration` (runtime object)

Built once; exposes:

- **`RepositoryRoot`** — Directory containing `engine_config.json`.
- **`PortMap` / `GetRequiredListenPort(portKey)`**
- **`ModulesInfos`** — Full module metadata including endpoints and per-row **`QwenOptions`** / **`GenericLlmProviderOptions`** (see `EngineModuleInfo` in `EngineConfigLoader.cs`) where applicable.
- **`ModuleAliases`** — Merged defaults + file overrides.
- **`TurnPipelines` / `GetRequiredTurnPipeline(id)`** — Validated turn pipeline definitions.
- **`ResolveProxyTargetModuleKey`**, **`FindModule`**, **`GetRequiredGenericLlmProviderModule()`** — Resolve **`generic_llm_provider`** to the concrete module row; provider code reads Ollama and **`num_ctx`** from that row’s option records.

## Token counting and budget telemetry

The module resolved from **`generic_llm_provider`** should expose **`POST /token_count`** when MemoryDirector budget telemetry is enabled. For `llm_provider_qwen`, the endpoint uses the configured **`default_chat_model`** and rejects mismatched caller model names rather than silently counting a different model.

`TokenCountResponse.exact` indicates whether the provider returned trusted token stats. `exact: false` means the value is a deterministic estimate, currently suitable for budgeting diagnostics but not tokenizer-grade accounting.

MemoryDirector persists context compiler telemetry as `pipeline_events` rows through `session_store`, not as `agent_messages`. This keeps diagnostics out of recall FTS while still allowing inspection through **`POST /memory/pipeline_events/recent`**.

## Contract examples and UI

`MainWindow` uses **`EngineConfiguration`** to populate HTTP test presets from each module’s **`endpoints`** list. Contract ids are opaque strings except where **`EngineContractExamples`** defines a sample JSON body.

When adding a new **`request_contract`**, extend **`EngineContractExamples.TryGetRequestBodyTemplate`** so the WPF preset dropdown can pre-fill a valid example.

## Build vs run

Runs expect **`launch.artifact`** paths to exist (build **`dotnet/MorpheusEngine.sln`** first). Missing artifacts throw **`FileNotFoundException`** at module start.
