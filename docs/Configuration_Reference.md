# Configuration reference

## `engine_config.json` (repository root)

Loaded by **`EngineConfigLoader.GetConfiguration()`**. The loader walks upward from the executable / current directory until it finds **`engine_config.json`**.

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

- **`llm_provider_qwen`**: **`ollama_port`** (required on that row only).
- **`intent_extractor`**: **`default_llm_model`** (required on that row only; also used as the Director’s chat model today via `EngineConfiguration.IntentDefaultLlmModel`).

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
- **`IntentDefaultLlmModel`**, **`LlmProviderOllamaListenPort`** — Derived from the special module rows above.

## Contract examples and UI

`MainWindow` uses **`EngineConfiguration`** to populate HTTP test presets from each module’s **`endpoints`** list. Contract ids are opaque strings except where **`EngineContractExamples`** defines a sample JSON body.

When adding a new **`request_contract`**, extend **`EngineContractExamples.TryGetRequestBodyTemplate`** so the WPF preset dropdown can pre-fill a valid example.

## Build vs run

Production-style runs expect **`launch.artifact`** paths to exist (build **`dotnet/MorpheusEngine.sln`** first). Missing artifacts throw **`FileNotFoundException`** at module start unless dev launch is enabled.
