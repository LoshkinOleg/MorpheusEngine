# Implementation Status

This file tracks what is currently implemented in code versus the target architecture.

## Snapshot

- Date: 2026-02-26
- Scope: runtime/backend/frontend/shared contracts

## Implemented now

- **Router + module-process architecture**
  - Backend API delegates turn orchestration to router, which calls standalone services over HTTP.
  - Evidence: `apps/backend/src/index.ts`, `apps/backend/src/router/orchestrator.ts`, `apps/backend/src/router/client.ts`

- **Standalone module services**
  - `intent_extractor`, `loremaster` (retrieve/pre/post), `default_simulator`, `arbiter`, and `proser` each run as separate apps.
  - Evidence: `apps/module-intent/src/index.ts`, `apps/module-loremaster/src/index.ts`, `apps/module-default-simulator/src/index.ts`, `apps/module-arbiter/src/index.ts`, `apps/module-proser/src/index.ts`

- **Per-run DB authority**
  - Session truth is `game_projects/<id>/saved/<runId>/world_state.db` (events + snapshots).
  - Evidence: `apps/backend/src/sessionStore.ts`

- **Shared inter-process contracts**
  - Module request/response envelopes and payload schemas are centralized in shared contracts.
  - Evidence: `packages/shared/src/schemas.ts`

- **Frontend DB-backed state**
  - Frontend reads run state via backend `GET /run/:runId/state`.
  - Evidence: `apps/frontend/src/api.ts`, `apps/frontend/src/App.tsx`

- **Debug inspection UX**
  - Turn selector, stepped pipeline timeline, and readable model conversation formatting are active.
  - Evidence: `apps/frontend/src/App.tsx`, `apps/frontend/src/styles.css`

- **Validation coverage**
  - Contract tests and router integration tests are present and passing.
  - Evidence: `tests/module-contracts.test.mjs`, `tests/router-integration.test.mjs`

## Partially implemented

- **Arbiter policy depth**
  - Arbiter is a standalone module but currently runs accept-first policy (no rerun/alternative selection behavior yet).
  - Evidence: `apps/module-arbiter/src/index.ts`, `apps/backend/src/router/orchestrator.ts`

- **Retriever sophistication**
  - Lore retrieval exists but uses simple deterministic chunk scoring, not semantic ranking.
  - Evidence: `apps/module-loremaster/src/index.ts`

- **Invariant library breadth**
  - Turn sequencing and schema contracts are enforced; richer world invariants remain limited.
  - Evidence: `apps/backend/src/index.ts`, `packages/shared/src/schemas.ts`

## Not yet implemented

- Authoritative classic simulation services (stealth/damage/economy/crew).
- Full deterministic authoritative/advisory merge policy engine beyond current fallback flow.
- Visibility/fog-of-war leak enforcement at world/view granularity.
- Rich provenance UI (citations panel) for retrieved lore.

## Current risks / drift watch

- Duplication risk: LLM provider/retry logic currently exists independently in each module service.
- Module binding currently maps to service URLs/env defaults; richer registry/provider indirection is still evolving.
- Retry/fallback behavior can still mask low-quality model output if warnings are ignored in debug review.
