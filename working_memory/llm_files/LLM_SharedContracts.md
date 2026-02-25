# LLM_SharedContracts

## Purpose

Track canonical shared schemas/contracts used between backend modules and prompts.

## Last verified

2026-02-25

## Owner / Source of truth

- Package: `packages/shared`
- Schema source: `packages/shared/src/schemas.ts`
- Human-readable interface map: `packages/shared/src/interfaces.ts`
- Prompt JSON-schema contracts: `packages/shared/src/promptContracts.ts`

## Invariants / Contracts (Facts)

- Zod schemas define authoritative shapes for core IR objects (`ActionCandidates`, `ProposedDiff`, `CommittedDiff`, etc.).
  - Evidence: `packages/shared/src/schemas.ts`
- Prompt output contracts are generated from Zod schemas (not hand-written JSON schema blobs).
  - Evidence: `packages/shared/src/promptContracts.ts`
- Shared package exports schemas, interfaces, and prompt contracts for backend consumption.
  - Evidence: `packages/shared/src/index.ts`

## Key entry points

- Contract definitions: `packages/shared/src/schemas.ts`
- Readable field docs: `packages/shared/src/interfaces.ts`
- Prompt constants: `packages/shared/src/promptContracts.ts`

## Operational workflows

- When changing a contract:
  - [ ] Update Zod schema in `schemas.ts`
  - [ ] Update corresponding interface docs in `interfaces.ts`
  - [ ] Update/confirm prompt contract export in `promptContracts.ts`
  - [ ] Build shared package (`npm run -w @morpheus/shared build`)
  - [ ] Run repo typecheck (`npm run typecheck`)

## Gotchas / footguns

- Editing backend prompt expectations without updating shared contracts creates hidden drift.
- Dist `.d.ts` readability is low by nature; prefer reading `src/interfaces.ts` for field intent.
- Type resolution errors can appear if shared package is not built before dependent checks.

## Recent changes

- Introduced `interfaces.ts` as readable contract companion to Zod schemas.
- Added prompt contract exports for intent extraction and proposed diff operations.
- Extended `ActionCandidate` with `consequenceTags` and `clarificationQuestion`.
- Added `LoremasterOutputSchema` / `LoremasterAssessmentSchema` contracts.
- Removed `ValidationReport` contract from active shared runtime surface for now.

## To verify

- Add schema version tags for debug payload compatibility over time.
- Add contract tests that fail when prompt contracts and schemas diverge unexpectedly.

## References / Links

- `docs/Conventions.md`
- `docs/DirectionGuardrails.md`
