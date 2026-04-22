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

### `modules[]` row shape

- **`port_key`**: Stable id; must have a matching **`ports.<port_key>`** entry.
- **`required`**: If true, engine startup waits for **`GET /health`** on that module.
- **`launch.artifact`**: Path to built `.exe` or `.dll` (relative to repo root unless absolute).
- **`launch.dev_project`**: Optional `.csproj` for `dotnet run` when **`MORPHEUS_DEV_LAUNCH`** is set.
- **`endpoints[]`**: Each **`path`**, **`method`** (`GET` or `POST`), optional **`description`**, **`request_contract`**, **`body_template`**.

**`request_contract`** ties into **`EngineContractExamples.TryGetRequestBodyTemplate`** in `EngineHttpContracts.cs` for UI samples / tooling.

### Module-specific optional fields

- **`llm_provider_qwen`**: **`ollama_port`** (required on that row only); **`default_chat_model`** (required on that row only; Ollama model for **`POST /chat`** and **`POST /generate`**).
- **`intent_extractor`**: optional **`default_llm_model`** (stored as **`EngineConfiguration.IntentDefaultLlmModel`** if present; not sent on proxied **`/generate`** — the Qwen module uses **`default_chat_model`**).

## Environment variables

| Variable | Effect |
|----------|--------|
| **`MORPHEUS_DEV_LAUNCH=1`** or **`true`** | `ManagedModule` launches **`dotnet run --project <dev_project>`** instead of the **`launch.artifact`** binary. |

## `EngineConfiguration` (runtime object)

Built once; exposes:

- **`RepositoryRoot`** — Directory containing `engine_config.json`.
- **`PortMap` / `GetRequiredListenPort(portKey)`**
- **`ModulesInfos`** — Full module metadata including endpoints.
- **`ModuleAliases`** — Merged defaults + file overrides.
- **`IntentDefaultLlmModel`** (optional legacy), **`LlmProviderOllamaListenPort`**, **`LlmProviderDefaultChatModel`** — Derived from the **`intent_extractor`** and **`llm_provider_qwen`** module rows.

## Contract examples and UI

`MainWindow` uses **`EngineConfiguration`** to populate HTTP test presets from each module’s **`endpoints`** list. Contract ids are opaque strings except where **`EngineContractExamples`** defines a sample JSON body.

When adding a new **`request_contract`**, extend **`EngineContractExamples.TryGetRequestBodyTemplate`** so the WPF preset dropdown can pre-fill a valid example.

## Build vs run

Production-style runs expect **`launch.artifact`** paths to exist (build **`dotnet/MorpheusEngine.sln`** first). Missing artifacts throw **`FileNotFoundException`** at module start unless dev launch is enabled.
