# LLM_RoadmapGuard

## Purpose

A compact direction checkpoint file: what we are intentionally building next, what we are explicitly not building yet, and how to detect accidental scope drift.

## Last verified

2026-02-25

## Owner / Source of truth

- Product/architecture intent: `docs/Overview.md`, `docs/Architecture.md`
- Current implementation reality: `docs/ImplementationStatus.md`

## Invariants / Contracts (Facts)

- Near-term target remains a single-player, modular text narrative engine with auditable turns.
  - Evidence: `docs/Overview.md`, `docs/Architecture.md`
- Non-prose modules are expected to operate on structured IR, not freeform text.
  - Evidence: `packages/shared/src/schemas.ts`, `apps/backend/src/engine.ts`
- Debug visibility of model conversations is considered a first-class requirement.
  - Evidence: `apps/backend/src/engine.ts` (`llmConversations`), `apps/frontend/src/App.tsx`

## Key entry points

- Direction docs: `docs/Overview.md`, `docs/DirectionGuardrails.md`
- Runtime reality: `docs/ImplementationStatus.md`
- Memory router: `working_memory/llm_files/LLM_Overview.md`

## Operational workflows

- Before starting a new feature:
  - [ ] Read `docs/DirectionGuardrails.md`
  - [ ] Confirm feature touches one of the declared near-term targets
  - [ ] If not, log the scope change in this file before coding
- After finishing a feature:
  - [ ] Update owning `LLM_*.md` file(s)
  - [ ] Update `docs/ImplementationStatus.md`
  - [ ] Add "To verify" items for unproven assumptions

## Gotchas / footguns

- Chasing UI polish before reinforcing deterministic backend invariants can hide core engine weaknesses.
- Adding new module types without explicit contracts quickly creates prompt drift and debugging ambiguity.
- Conflating "works once" with "operationally stable" leads to poor long-session behavior.

## Recent changes

- Established baseline memory map and implementation/direction docs for anti-drift review.

## To verify

- Priority ordering for next implementation wave:
  - contract tests
  - richer deterministic invariant/policy checks
  - module policy enforcement (authoritative vs advisory)
  - debug UI ergonomics

## References / Links

- `docs/DirectionGuardrails.md`
- `docs/ImplementationStatus.md`
- `working_memory/changelogs/LLM_Changelog_0001-memory-baseline-and-direction-guards.md`
- `working_memory/changelogs/LLM_Changelog_0002-loremaster-placeholder-and-validator-removal.md`
