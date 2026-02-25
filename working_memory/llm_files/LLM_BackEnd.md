# LLM_BackEnd

## Purpose

Track backend runtime contracts, turn processing flow, logging/session persistence, and model-provider interaction boundaries.

## Last verified

2026-02-25

## Owner / Source of truth

- Primary code: `apps/backend/src/index.ts`, `apps/backend/src/engine.ts`, `apps/backend/src/logs.ts`
- Shared contracts: `packages/shared/src/schemas.ts`, `packages/shared/src/promptContracts.ts`

## Invariants / Contracts (Facts)

- API endpoints currently include:
  - `GET /health`
  - `GET /game_projects/:id`
  - `GET /game_projects/:id/sessions`
  - `POST /run/start`
  - `GET /run/:runId/logs`
  - `POST /run/:runId/open-saved-folder`
  - `POST /turn`
  - Evidence: `apps/backend/src/index.ts`
- Turn payload requires `runId`, `turn`, `playerInput`, `playerId`.
  - Evidence: `apps/backend/src/index.ts` `/turn` route body checks
- Turn sequencing is enforced (`turn === max(snapshot.turn)+1`).
  - Evidence: `apps/backend/src/index.ts` `/turn` route conflict check
- Non-prose model outputs are schema-validated with retries and fallbacks.
  - Evidence: `apps/backend/src/engine.ts` `generateStructuredWithRetries()`
- Turn response/trace uses `warnings` (no standalone validator module in current runtime).
  - Evidence: `apps/backend/src/engine.ts` `processTurn()` return/trace payload
- `loremaster` placeholder stage produces structured assessments per action candidate.
  - Evidence: `apps/backend/src/engine.ts` `runLoremasterPlaceholder()`, `packages/shared/src/schemas.ts` `LoremasterOutputSchema`
- `default_simulator` invariant `moduleName` is code-stamped (model is not trusted for this invariant).
  - Evidence: `apps/backend/src/engine.ts` `runDefaultSimulator()`
- Session logs are persisted under `game_projects/<id>/saved/<session_id>/`.
  - Evidence: `apps/backend/src/engine.ts` and `apps/backend/src/logs.ts`

## Key entry points

- Request orchestration: `apps/backend/src/index.ts`
- Turn loop core: `apps/backend/src/engine.ts`
- Session log read/write/list helpers: `apps/backend/src/logs.ts`
- Provider creation: `apps/backend/src/providers/factory.ts`
- Ollama provider: `apps/backend/src/providers/ollama.ts`

## Operational workflows

- Local backend run:
  - [ ] Set backend env vars (`apps/backend/.env` from `.env.example`)
  - [ ] Run `npm run dev:backend`
  - [ ] Check `GET /health`
- Quick API smoke test:
  - [ ] `POST /run/start`
  - [ ] `POST /turn`
  - [ ] `GET /run/:runId/logs`

## Gotchas / footguns

- Stub provider causes structured stages to fallback often; check turn `warnings` before trusting output quality.
- If session folders are manually edited/deleted, UI session list and DB run rows can diverge by design.
- Never trust model output for invariants that can be set in code.

## Recent changes

- Added per-module `llmConversations` tracing in turn debug payload.
- Added per-session log storage (`saved/<session_id>/`) and disk-based session listing.
- Added endpoint to open selected session folder from UI.
- Added a deterministic `loremaster` placeholder stage and surfaced it in debug traces.
- Replaced `validation` turn payload field with `warnings` and removed validator module usage.

## To verify

- Add deterministic invariant validators beyond turn ordering (inventory bounds, anchor uniqueness, etc.).
- Add explicit authoritative/advisory resolver merge policy enforcement in code (currently documented, partially represented).

## References / Links

- `docs/Architecture.md`
- `docs/Conventions.md`
- `docs/DirectionGuardrails.md`
