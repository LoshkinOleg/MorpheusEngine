# Implementation Status

This file tracks what is currently implemented in code versus the target architecture.

## Snapshot

- Date: 2026-02-25
- Scope: runtime/backend/frontend/shared contracts

## Implemented now

- **Monorepo + build**
  - Workspaces for backend/frontend/shared are active.
  - Evidence: `package.json`, `apps/*/package.json`, `packages/shared/package.json`
  - Verify: `npm run typecheck`, `npm run build`

- **Backend runtime shell**
  - Express server, SQLite init, run start, turn processing.
  - Evidence: `apps/backend/src/index.ts`, `apps/backend/src/db.ts`, `apps/backend/src/engine.ts`
  - Verify: `GET /health`, `POST /run/start`, `POST /turn`

- **No standalone validator module (current policy)**
  - Turn payloads expose `warnings` and do not include a validator stage/module.
  - Evidence: `apps/backend/src/engine.ts`, `apps/frontend/src/api.ts`, `game_projects/sandcrawler/manifest.json`

- **LLM provider abstraction**
  - Stub + Ollama providers with env-based selection.
  - Evidence: `apps/backend/src/providers/factory.ts`, `apps/backend/src/providers/ollama.ts`
  - Verify: `LLM_PROVIDER=ollama`, `npm test`

- **Structured turn trace**
  - Intent/loremaster/proposal/commit/warnings/narration and provider conversation traces are persisted.
  - Evidence: `apps/backend/src/engine.ts` (`turnTracePayload`, `llmConversations`)
  - Verify: inspect `game_projects/<id>/saved/<session_id>/debug.log`

- **Session file model**
  - Per-session logs under `game_projects/<id>/saved/<session_id>/`.
  - Evidence: `apps/backend/src/engine.ts`, `apps/backend/src/logs.ts`
  - Verify: start run, submit turn, inspect created folder and files

- **Frontend file-backed UI**
  - Game UI + Debug UI render from log files via backend APIs.
  - Evidence: `apps/frontend/src/App.tsx`, `apps/frontend/src/api.ts`
  - Verify: run app, observe views update after `/run/:runId/logs`

- **Debug inspection UX**
  - Turn selector, up/down navigation, structured debug cards, model conversation visibility.
  - Evidence: `apps/frontend/src/App.tsx`
  - Verify: submit multiple turns, browse turns in Debug UI

## Partially implemented

- **Intent extraction**
  - LLM call + schema validation + retries exists, but fallback behavior is common in stub mode.
  - Evidence: `apps/backend/src/engine.ts::extractIntent()`

- **Clarification turn policy**
  - Runtime now has an explicit clarification branch driven by consequence tags (`needs_clarification`, `no_target_in_scope`) and optional `clarificationQuestion`.
  - Evidence: `packages/shared/src/schemas.ts`, `apps/backend/src/engine.ts`

- **Loremaster placeholder stage**
  - Deterministic placeholder `loremaster` stage runs each turn and emits structured assessments with status/tags/rationale.
  - Evidence: `packages/shared/src/schemas.ts::LoremasterOutputSchema`, `apps/backend/src/engine.ts::runLoremasterPlaceholder()`

- **Default simulation**
  - LLM operations proposal + deterministic invariant stamping (`moduleName`) exists.
  - Evidence: `apps/backend/src/engine.ts::runDefaultSimulator()`

- **Narration**
  - Proser call exists with leak-safe fallback derived from committed observation.
  - Evidence: `apps/backend/src/engine.ts::generateNarration()`

## Not yet implemented

- Lore retrieval module and world-lore ranking pipeline.
- Full LoreMaster LLM-based plausibility critic (current stage is deterministic placeholder).
- Arbiter conflict resolution across multiple resolvers (authoritative + advisory merge policy).
- Deterministic invariant/policy check library beyond current turn sequencing checks.
- Rich test coverage for endpoint edge cases and module misuse cases.

## Current risks / drift watch

- Prompt content still partly hand-authored in backend logic (though schema contracts are centralized in shared).
- Session listing is disk-based; DB and disk can diverge by design if files are manually altered.
- Retry/fallback behavior may mask low-quality model outputs if warning inspection is skipped.
