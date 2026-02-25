# Morpheus Engine

Modular LLM narrative game engine scaffold (AI Dungeon-like) with:

- Node.js + TypeScript backend orchestrator
- React + Vite frontend shell
- Shared JSON schema package
- SQLite event/snapshot persistence
- Data-driven game project format with a sample sandcrawler project

## Monorepo layout

- `apps/backend` - orchestrator runtime and APIs
- `apps/frontend` - chat/menu/debug web UI shell
- `packages/shared` - core schemas and shared types
- `game_projects/sandcrawler` - example game project
- `working_memory/LLM_LlmFilesUsage.md` - example LLM Agent working memory file

## Quick start

1. Install dependencies:
   - `npm install`
2. Run backend + frontend together:
   - `npm run dev`
3. Optional separate terminals:
   - backend: `npm run dev:backend`
   - frontend: `npm run dev:frontend`

Backend defaults to `http://localhost:8787`.
Frontend defaults to `http://localhost:5173`.

## Backend API (MVP)

- `GET /health`
- `GET /game_projects/:id`
- `GET /game_projects/:id/sessions`
- `POST /run/start`
- `GET /run/:runId/logs`
- `POST /run/:runId/open-saved-folder`
- `POST /turn`

### Turn payload

```json
{
  "runId": "uuid",
  "turn": 1,
  "playerInput": "I order the crew to seal the hull breach.",
  "playerId": "entity.player.captain"
}
```

## Notes

- The current pipeline is a runnable skeleton with deterministic placeholder modules.
- Shared schemas include: `ActionCandidates`, `LoremasterOutput`, `ProposedDiff`, `CommittedDiff`, and `ObservationEvent`.
- `game_projects` are folder-based for now and can be zipped later for distribution.
- Session logs are persisted per run under `game_projects/<id>/saved/<session_id>/`.
- Frontend state is file-backed from session logs via backend APIs.

## Documentation map

- `docs/Overview.md` - high-level product and runtime goals.
- `docs/Architecture.md` - module graph, state model, and turn processing details.
- `docs/Conventions.md` - naming, contracts, and coding rules.
- `docs/ImplementationStatus.md` - implemented systems vs target architecture checkpoints.
- `docs/DirectionGuardrails.md` - practical anti-drift checks for upcoming changes.

## LLM Agents

- Any LLM coding agents that come across this README.md need to familiarize themselves with the following file: working_memory/LLM_LlmFilesUsage.md which details the functioning of the repo-wide long term context memory files usage.
- For persistent project memory, use the `working_memory/llm_files/LLM_*.md` files.