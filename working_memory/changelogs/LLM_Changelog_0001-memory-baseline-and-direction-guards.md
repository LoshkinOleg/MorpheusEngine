# LLM Changelog 0001 - Memory baseline and direction guards

## Date

2026-02-25

## Status

Accepted

## Scope

Initial documentation hardening pass after establishing a runnable skeleton app.

## Decision summary (what changed)

- Created a first-class `working_memory/llm_files/` memory baseline:
  - `LLM_Overview.md`
  - `LLM_BackEnd.md`
  - `LLM_FrontEnd.md`
  - `LLM_SharedContracts.md`
  - `LLM_GameProject_Sandcrawler.md`
  - `LLM_RoadmapGuard.md`
- Added implementation-vs-target visibility docs:
  - `docs/ImplementationStatus.md`
  - `docs/DirectionGuardrails.md`
- Updated `README.md` to reflect current startup flow and documentation map.

## Frontend implementation snapshot (done so far)

- Moved to a file-backed session model where UI state is refreshed from backend log endpoints instead of synthetic local-only placeholders.
- Split UI into stacked Game UI (top) and Debug UI (bottom) for simultaneous visibility while testing turns.
- Added session selector backed by disk sessions (`saved/<session_id>`), defaulting to the active session.
- Added debug turn navigation (turn selector + step controls) and structured per-turn debug cards.
- Exposed full model conversation traces in Debug UI (`intent_extractor`, `default_simulator`, `proser`) for prompt/response transparency.
- Added control to open the currently selected session's saved folder from the frontend.

## Why this decision was made

- Context-window limits made continuity fragile across long sessions; we needed persistent, evidence-backed memory files to avoid re-discovery loops.
- The codebase reached a point where “implemented now” and “planned architecture” could drift silently without explicit checkpoints.
- Debuggability is a core product value for this engine; documentation must make hidden assumptions and drift visible early.

## Alternatives considered

- **Keep memory in one large file** (`LLM_Todo.md` style)
  - Rejected: mixes operational checklist content with durable architecture memory; hard to scan and harder to keep truthful.
- **Only rely on docs/** and avoid `LLM_*.md`
  - Rejected: docs alone do not provide the compact per-subsystem memory routing needed for LLM-assisted iteration.

## Consequences

- **Positive**
  - Faster onboarding/re-entry for future sessions.
  - Clear ownership boundaries for “current truth” vs “decision history.”
  - Practical anti-drift checklists before major merges.
- **Trade-offs**
  - Ongoing maintenance overhead: memory files and status docs must be updated after behavior changes.
  - Risk of stale claims if update protocol is skipped.

## Validation evidence

- Policy basis:
  - `working_memory/LLM_LlmFilesUsage.md` (sections on LLM_Changelog usage and update protocol)
- Created files:
  - `working_memory/llm_files/*.md`
  - `docs/ImplementationStatus.md`
  - `docs/DirectionGuardrails.md`
- Runtime contract alignment:
  - `apps/backend/src/index.ts`
  - `apps/backend/src/engine.ts`
  - `apps/frontend/src/App.tsx`
  - `apps/frontend/src/api.ts`
  - `apps/frontend/src/styles.css`
  - `packages/shared/src/schemas.ts`

## Follow-up actions

- Keep this changelog immutable except status corrections or typo fixes.
- For future architectural choices, append `LLM_Changelog_0002+` entries and link them from owning `LLM_*.md` files.
