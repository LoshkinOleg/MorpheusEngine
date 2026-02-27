# LLM_BackEnd

## Purpose

Track backend API/router contracts, per-run DB persistence, and module-service orchestration boundaries.

## Last verified

2026-02-26

## Owner / Source of truth

- Primary code: `apps/backend/src/index.ts`, `apps/backend/src/router/*`, `apps/backend/src/sessionStore.ts`
- Shared contracts: `packages/shared/src/schemas.ts`

## Invariants / Contracts (Facts)

- API endpoints currently include:
  - `GET /health`
  - `GET /game_projects/:id`
  - `GET /game_projects/:id/sessions`
  - `POST /run/start`
  - `GET /run/:runId/state`
  - `GET /run/:runId/turn/:turn/pipeline`
  - `POST /run/:runId/open-saved-folder`
  - `POST /turn`
  - `POST /turn/step/start`
  - `POST /turn/step/next`
- `POST /turn` delegates to router orchestration (`processTurnViaRouter`) and does not run stage logic inline.
- Session authority is per-run DB: `game_projects/<id>/saved/<runId>/world_state.db`.
- Turn sequencing is enforced (`turn === max(snapshot.turn)+1`) before router execution.
- Turn trace payload persists `llmConversations` and stage outputs in `events` (`module_trace`).
- Step execution state is persisted in `turn_execution` and ordered stage events in `pipeline_events`.
- Arbiter is now a standalone module stage (`/invoke`) before proser; world-state persistence is a separate final stage.

## Key entry points

- API routes + lifecycle: `apps/backend/src/index.ts`
- Router orchestration: `apps/backend/src/router/orchestrator.ts`
- Module HTTP client + schema parse boundaries: `apps/backend/src/router/client.ts`
- Module binding -> URL resolution: `apps/backend/src/router/registry.ts`
- Session DB read/write/listing: `apps/backend/src/sessionStore.ts`

## Operational workflows

- Local backend run:
  - [ ] Set backend env vars (`apps/backend/.env` from `.env.example`)
  - [ ] Run `npm run dev:backend`
  - [ ] Check `GET /health`
- Quick API smoke test:
  - [ ] `POST /run/start`
  - [ ] `POST /turn` (normal mode) or `POST /turn/step/start` + repeated `POST /turn/step/next`
  - [ ] `GET /run/:runId/turn/:turn/pipeline`
  - [ ] `GET /run/:runId/state`

## Gotchas / footguns

- Module URLs come from env defaults or manifest-resolved bindings; wrong URLs fail turn execution.
- Session discovery is folder-based (`saved/<runId>/world_state.db`), so manual file edits can break run lookup.
- Router assumes module responses strictly match shared schemas; non-conforming responses fail fast.

## Recent changes

- Replaced in-process backend stage execution with router -> standalone module service calls.
- Removed backend module debug endpoints; isolation testing is done by direct module service calls.
- Replaced log-file-backed session state with DB-backed `GET /run/:runId/state`.
- Added standalone `module-arbiter` and moved arbiter decision to explicit module stage before proser.

## To verify

- Add richer deterministic invariant checks post-commit (beyond turn ordering).
- Add authoritative/advisory merge policy enforcement for multi-resolver future modules.

## References / Links

- `docs/Architecture.md`
- `docs/Conventions.md`
- `docs/DirectionGuardrails.md`
