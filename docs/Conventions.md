# Morpheus Engine Conventions

This document defines coding style, naming conventions, and API contract rules for the Morpheus Engine monorepo.

## Core principles

- **Serviceability first**: optimize for readability, debuggability, and low operational risk.
- **KISS**: prefer the simplest correct solution.
- **YAGNI**: do not introduce abstractions before a second real use case exists.
- **Fail fast**: validate inputs and return explicit, structured errors.
- **Traceability**: each turn should be inspectable via persisted events/module traces.

## Naming conventions

- **TypeScript symbols**: `camelCase` for variables/functions, `PascalCase` for types/classes/interfaces.
- **Filesystem**:
  - Backend router/API modules: descriptive lowercase files like `gameProject.ts`, `sessionStore.ts`, `router/orchestrator.ts`.
  - Standalone module services live in `apps/module-*`.
  - Game content: `game_projects/<project_id>/...`.
- **Module IDs** (pipeline): use `snake_case` identifiers such as `intent_extractor`, `lore_retriever`, `loremaster`, `default_simulator`, `arbiter`, `proser`.
- **Fact keys**:
  - `core.*` reserved for engine-level keys.
  - `world.*` for first-party game project facts.
  - `mod.<author>.<pack>.*` for third-party mods.

## API conventions

- **Route naming**: use plural resource routes where possible (for example `GET /game_projects/:id`).
- **Request/response payloads**: use `camelCase` JSON keys for current backend endpoints.
- **Error envelope**: return a consistent shape:
  - `error.code` (machine readable)
  - `error.message` (client safe)
  - `error.requestId` (correlation id)
  - `error.details` (optional structured context)
- **Validation order**:
  1. Parse request and validate required top-level fields.
  2. Validate schema/shape (Zod/shared schemas).
  3. Run business logic.
  4. Persist events/snapshots.
  5. Return structured result or structured error.

## Backend module design

- Keep functions focused: validation, orchestration, persistence, and transformation should be split when complexity grows.
- Export only what is used by other modules.
- Prefer pure functions for module proposal generation where possible.
- Treat module output as structured IR (`ActionCandidates`, `LoremasterOutput`, `ProposedDiff`, `CommittedDiff`), not free-form text.
- For deterministic authoritative gameplay modules, do not implement arbiter "retry for different result" behavior on identical inputs.
  - Valid arbiter rejection reasons: schema/contract error, invariant violation, stale input/state version mismatch.

## Comments and documentation

- Add comments for non-obvious logic, assumptions, and invariants.
- Avoid comments that merely restate syntax.
- Keep `README.md` high-level and task-oriented.
- Put implementation rules and conventions in `docs/` (this file).

## Testing conventions

- Prioritize tests around contracts and behavior, not private implementation details.
- Add misuse tests for exported functions:
  - missing arguments
  - wrong types
  - malformed payloads
  - invalid enum values
- For endpoint tests, cover:
  - happy path
  - validation errors
  - invariant violations
  - persistence side effects (events/snapshots written as expected)

## Logging and debug trace conventions

- Keep startup and operational logs single-line and parse-friendly where possible.
- Include request/run context in logs (`runId`, `turn`, module name).
- Preserve module trace payloads in event log for post-turn inspection.

## Configuration conventions

- Environment variables use upper snake case (for example `GAME_PROJECT_ID`, `PORT`).
- Do not hardcode secrets in code or docs.
- Prefer explicit defaults for local development, but keep production-sensitive values configurable.
