# Configuration reference

Last reviewed: 2026-05-08.

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
- **That resolved row** must carry **`GenericLlmProviderOptions`** (**`num_ctx`** from the generic provider JSON). When the concrete key is **`llm_provider_qwen`**, the same row must also have **`QwenOptions`** (**`ollama_port`**, **`default_chat_model`**, optional **`filter_off_noisy_logs`**, optional **`thinking`**).
- **`thinking`** (optional, default false): When **`true`** on **`llm_provider_qwen`**, Morpheus forwards **`think: true`** on Ollama **`/api/chat`** and **`/api/generate`**, and **`memory_director`** requires a non-empty JSON **`thought`** on each action (schema, parser, and agent-prompt line). When **`false`**, forwards **`think: false`**, omits **`thought`** from the director action JSON schema sent as Ollama **`format`**, does not require **`thought`** in **`TryParseAction`**, strips the “brief in `thought`” line from the loaded agent prompt, and adds a system rule that actions must include only **`tool`** and **`arguments`**. Only **`llm_provider_qwen`** may set this field.
- **`filter_off_noisy_logs`** (optional, default false): When **`true`** on **`llm_provider_qwen`**, bundled Ollama child stdout/stderr is post-processed before console logging (phase 1: deduplicated TRAFFIC channels, collapsed tensor/KV/embedding bursts, PATH redaction; phase 2: print_info / llama_context / device memory / nomic-embed summaries, JSON status dedupe, PRIME aggregation). Only **`llm_provider_qwen`** may set this field. See **`dotnet/src/MorpheusEngine.LlmProvider_qwen/LLM_OllamaLogFiltering.md`**.
- **`GET /debug/last_llm_payload`** on the **`generic_llm_provider`** module returns the last Ollama wire JSON from **`POST /chat`**, **`POST /generate`**, or **`POST /summarize`** (summarize is recorded as generate-shaped payload). MorpheusEngine.App Context Inspector fetches it via **`router` `POST /proxy`** and displays a reduced view (chat: **`messages`** only; generate: **`prompt`** / **`system`** only), omitting **`model`**, **`stream`**, **`truncate`**, **`options`**, **`format`**, and **`keep_alive`**.
- **`POST /summarize`** on **`generic_llm_provider`** (`llm_provider_qwen`): episodic recall compression via Ollama **`/api/generate`**, **`default_chat_model`**, bundled **`prompts/summarize_system.md`**, and caller **`content`** (MemoryDirector post-turn compaction). No action JSON schema.
- **`module_aliases.generic_embeddings`** must point at a concrete **`port_key`** (typically **`embeddings_ollama`**). The resolved row must carry **`EmbeddingsModuleOptions`**: **`ollama_port`**, **`default_embedding_model`**, **`keep_model_loaded_for`**, and **`embeddings_num_ctx`** (integer **256–131072**, forwarded to Ollama as **`options.num_ctx`** on `/api/embed`; use a value that matches the embedding model’s trained context, e.g. **2048** for **`nomic-embed-text`**). No other module row may set **`embeddings_num_ctx`**.

## `EngineConfiguration` (runtime object)

Built once; exposes:

- **`RepositoryRoot`** — Directory containing `engine_config.json`.
- **`PortMap` / `GetRequiredListenPort(portKey)`**
- **`ModulesInfos`** — Full module metadata including endpoints and per-row **`QwenOptions`** / **`GenericLlmProviderOptions`** / **`EmbeddingsModuleOptions`** (see `EngineModuleInfo` in `EngineConfigLoader.cs`) where applicable.
- **`ModuleAliases`** — Merged defaults + file overrides.
- **`TurnPipelines` / `GetRequiredTurnPipeline(id)`** — Validated turn pipeline definitions.
- **`ResolveProxyTargetModuleKey`**, **`FindModule`**, **`GetRequiredGenericLlmProviderModule()`** — Resolve **`generic_llm_provider`** to the concrete module row; provider code reads Ollama and **`num_ctx`** from that row’s option records.
- **`GetRequiredGenericEmbeddingsModule()`** — Resolve **`generic_embeddings`**; embeddings provider reads **`embeddings_num_ctx`** (and Ollama/model fields) from that row’s **`EmbeddingsOptions`**.

## Token counting and budget telemetry

The module resolved from **`generic_llm_provider`** should expose **`POST /token_count`** when MemoryDirector budget telemetry is enabled. For `llm_provider_qwen`, the endpoint uses the configured **`default_chat_model`** and rejects mismatched caller model names rather than silently counting a different model.

**Request shape:** exactly one of non-empty **`text`** (raw `/api/generate` probe; embeddings) or non-empty **`messages[]`** (chat-aligned `/api/chat` probe with `num_predict: 0`). MemoryDirector sends **`messages`**, optional **`format`** (action schema), and **`keepAlive`** — same wire as **`POST /chat`**.

`TokenCountResponse.exact` indicates whether the provider returned trusted token stats from Ollama `prompt_eval_count`. For `llm_provider_qwen`, a missing `prompt_eval_count` yields HTTP **502** (no char/utf8 estimate). **MemoryDirector** compile requires `exact: true` and throws if `/token_count` fails or is non-exact. Before each `/chat`, MemoryDirector may run multiple **`POST /summarize`** passes (pre-flight compaction) to fold oldest prior turns until context is within `targetContextTokens` (`num_ctx * target_context_ratio`, default ratio **0.7**). If still over budget after safe folds, compile throws and the turn fails before `/chat` is invoked.

**`target_context_ratio`** on the **`generic_llm_provider`** module row (optional, default **0.7**): fraction of **`num_ctx`** used as the working-memory token target. Must yield `1 <= targetContextTokens < num_ctx`.

MemoryDirector persists context compiler telemetry as `pipeline_events` rows through `session_store`, not as `agent_messages`. Per-step compile accounting uses **`persist_step.contextAccounting`**; compaction passes use **`persist_step.diagnosticsJson`** (JSON with `compactionPhase`, `passIndex`, fold range, and pre-compaction `accounting`). Inspect via **`POST /memory/pipeline_events/recent`** (`eventType: memory_context_budget`).

## Contract examples and UI

`MainWindow` uses **`EngineConfiguration`** to populate HTTP test presets from each module’s **`endpoints`** list. Contract ids are opaque strings except where **`EngineContractExamples`** defines a sample JSON body.

When adding a new **`request_contract`**, extend **`EngineContractExamples.TryGetRequestBodyTemplate`** so the WPF preset dropdown can pre-fill a valid example.

## Build vs run

Runs expect **`launch.artifact`** paths to exist (build **`dotnet/MorpheusEngine.sln`** first). Missing artifacts throw **`FileNotFoundException`** at module start.
