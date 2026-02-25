# LLM Changelog 0002 - Loremaster placeholder and validator removal

## Date

2026-02-25

## Status

Accepted

## Scope

Runtime and docs alignment pass after introducing loremaster placeholder and clarification tags.

## Decision summary (what changed)

- Added a deterministic `loremaster` placeholder stage into the backend turn flow.
- Extended shared action contracts with consequence tags and optional clarification questions.
- Added clarification-branch behavior that can skip simulation/proser and ask targeted follow-up questions.
- Removed standalone validator module usage for now:
  - removed validator module binding from `game_projects/sandcrawler/manifest.json`
  - replaced turn payload `validation` with `warnings`
  - removed active `ValidationReport` contract from shared schemas
- Updated docs and LLM memory files to reflect current runtime truth.

## Why this decision was made

- Needed a concrete, inspectable plausibility stage (`loremaster`) without committing to full LLM critic complexity yet.
- Clarification behavior required explicit contract fields to avoid ambiguous intent handling.
- Validator module language and runtime behavior were diverging; removing it for now reduces conceptual drift and implementation mismatch.

## Alternatives considered

- Keep validator module as a no-op placeholder.
  - Rejected: increased confusion by implying a stage that was not meaningfully implemented.
- Add full LLM-based loremaster immediately.
  - Rejected: higher complexity than needed for current MVP step; deterministic placeholder is safer and easier to debug.

## Consequences

- **Positive**
  - Pipeline is easier to reason about and matches current code behavior.
  - Clarification flow is explicit in contracts and runtime traces.
  - Debug UI now shows loremaster payloads with consistent warning-based status.
- **Trade-offs**
  - Loss of a dedicated validator abstraction in this phase.
  - Invariant/policy checks remain incremental and distributed rather than centralized.

## Validation evidence

- Backend/runtime:
  - `apps/backend/src/engine.ts`
  - `apps/backend/src/index.ts`
- Frontend contract/view:
  - `apps/frontend/src/api.ts`
  - `apps/frontend/src/App.tsx`
- Shared contracts:
  - `packages/shared/src/schemas.ts`
  - `packages/shared/src/interfaces.ts`
- Game project binding:
  - `game_projects/sandcrawler/manifest.json`
- Documentation alignment:
  - `docs/Overview.md`
  - `docs/Architecture.md`
  - `docs/ImplementationStatus.md`
  - `working_memory/llm_files/LLM_*.md`

## Follow-up actions

- Revisit centralized invariant/policy checks after authoritative modules are added.
- If reintroducing validator later, define narrow scope and explicit contracts before wiring.
