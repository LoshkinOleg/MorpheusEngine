# LLM_Overview

## Purpose

Routing/index file for LLM memory in this repository. Use this first, then jump to owning subject files.

## Last verified

2026-02-26

## Owner / Source of truth

- Source of truth for this index: `working_memory/llm_files/LLM_Overview.md`
- Memory file policy: `working_memory/LLM_LlmFilesUsage.md`

## Invariants / Contracts (Facts)

- LLM memory files live under `working_memory/llm_files/`.
  - Evidence: `working_memory/LLM_LlmFilesUsage.md` section "Where they live"
- Core implementation is split across backend, frontend, shared contracts, and game project data.
  - Evidence: `README.md` monorepo layout section
- Run state is persisted per run under `game_projects/<id>/saved/<session_id>/world_state.db`.
  - Evidence: `apps/backend/src/sessionStore.ts`

## Key entry points

- Architecture + direction:
  - `docs/Overview.md`
  - `docs/Architecture.md`
  - `docs/Conventions.md`
  - `docs/ImplementationStatus.md`
  - `docs/DirectionGuardrails.md`
- Subject memory files:
  - `working_memory/llm_files/LLM_BackEnd.md`
  - `working_memory/llm_files/LLM_FrontEnd.md`
  - `working_memory/llm_files/LLM_GameProject_Sandcrawler.md`

## Operational workflows

- Startup reading order for LLM agents:
  - [ ] Read `working_memory/LLM_LlmFilesUsage.md`
  - [ ] Read this file (`LLM_Overview.md`)
  - [ ] Read owning subject file(s) for task area
  - [ ] Then inspect code and make changes

## Gotchas / footguns

- Do not treat stale in-memory UI placeholders as source of truth; player/debug views are DB-backed via backend state projection.
- Do not treat DB run rows as authoritative session list for UI; session selector is disk-backed (`saved/*` folders).

## Recent changes

- Added initial `LLM_*.md` memory baseline for backend/frontend/game-project tracking.
- Migrated runtime to router + standalone module services (`apps/module-*`).
- Removed standalone validator module usage from runtime/docs for now.
- Added explicit standalone arbiter module stage and stepped pipeline timeline/docs updates.

## To verify

- Whether additional subject files are needed now (for example `LLM_Testing.md` or `LLM_ProviderRouting.md`) as complexity grows.

## References / Links

- `working_memory/LLM_LlmFilesUsage.md`
- `working_memory/changelogs/LLM_Changelog_0001-memory-baseline-and-direction-guards.md`
- `working_memory/changelogs/LLM_Changelog_0002-loremaster-placeholder-and-validator-removal.md`
- `README.md`
- `docs/ImplementationStatus.md`
