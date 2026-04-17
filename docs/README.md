# MorpheusEngine documentation

Short-form architecture notes for humans and LLM agents. For narrative design experiments with claude.ai website, see `docs/ClaudeDMSession/`.

| Document | Purpose |
|----------|---------|
| [LLM_QuickReference.md](LLM_QuickReference.md) | Dense facts: paths, ports, pipeline, where to change what (start here for LLM agents). |
| [Architecture_Overview.md](Architecture_Overview.md) | Repository layout, processes, and how the WPF host relates to the engine. |
| [Modules_HTTP_and_Router.md](Modules_HTTP_and_Router.md) | Each backend module, HTTP surface, router `/turn` and `/proxy`. |
| [GameProjects_and_SessionStore.md](GameProjects_and_SessionStore.md) | `game_projects/` layout, SQLite run DB, lore seeding, turn sequencing. |
| [Configuration_Reference.md](Configuration_Reference.md) | `engine_config.json`, port keys, dev launch, shared contracts. |

Source of truth for JSON shapes remains `dotnet/src/MorpheusEngine.Core/EngineHttpContracts.cs` and `engine_config.json` at the repository root.
