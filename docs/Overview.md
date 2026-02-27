# Morpheus Engine Overview

Morpheus Engine is a modular narrative game engine for interactive text experiences (AI Dungeon-like), designed around a router that orchestrates specialized module processes.

## Project goals

- Build a single-player narrative runtime where player actions drive evolving world state.
- Combine LLM-driven modules (interpretation, plausibility, prose, fallback simulation) with classic deterministic modules (damage, stealth, economy, crew).
- Keep game content data-driven by default, with optional code modules for advanced simulations.
- Support both online and local model providers behind the same provider interface.

## Design principles

- **Soft resolution over refusal**: improbable actions should resolve into plausible outcomes, not hard "cannot do" responses.
    - Example: if the player enters "I kill the skeleton by shooting it with my bow!", the outcome should be "You shoot the skeleton with your bow but the arrow bounces off." and not "You can't do that, skeletons are immune to piercing damage."
- **Traceable turns**: every turn should be inspectable end-to-end through event logs and module traces via the Debug UI.
- **Traceable pipeline order**: debug should present ordered stage-by-stage events, not only aggregate dumps.
- **Mod-as-full-game**: each game project can define lore, tables, module bindings, and optional plugin code.
- **Fair hidden information**: world truth and player-visible truth are separate to support stealth and discovery gameplay.
- **LLMs propose; engine commits**: LLM modules propose structured outputs, but final state changes are committed only by engine-controlled arbiter.
- **Fairness for authoritative mechanics**: arbiter must not rewrite authoritative domain results.
    - For deterministic/classical authoritative modules, arbiter can only accept or reject with a contract/invariant error (same input should produce the same output, so "retry for a different result" is invalid).
    - For LLM-based authoritative modules, bounded rerun/correction is allowed only for malformed or policy-violating outputs, not for narrative preference.

## LLM module model (high level)

Morpheus uses multiple focused LLM modules instead of a single "do everything" model:

- **Intent Extractor**: turns player text into structured action candidates.
- **LoreMaster**: evaluates plausibility and consequence tags.
- **Simulator(s)**: proposes state diffs when no authoritative classic module exists.
- **Arbiter**: selects/merges proposals into final committed state diffs.
- **Proser**: produces final player-facing prose from committed state only.

Core contract:

- Structured JSON IR first; freeform text only in Proser.
- Player text and retrieved lore are treated as untrusted content.
- Schema/invariant failures use bounded retries and safe fallbacks.
- If clarification is needed, engine returns a clarification question and commits minimal/no world changes for that turn.
  - Example target behavior: if the player is in an empty room and inputs `attack`, the engine should ask `What do you want to attack?` instead of inventing a target or forcing a full simulation commit.

## What exists today (MVP scaffold)

- `apps/backend`: Node.js + TypeScript API + router orchestrator with per-run SQLite persistence.
- `apps/frontend`: React + Vite UI with stacked Game UI + Debug UI, session selector, and DB-backed state.
- `packages/shared`: shared schemas/types for structured module IR.
- `game_projects/sandcrawler`: example game project with manifest, lore, and tables.
- standalone module services under `apps/module-*` (`intent`, `loremaster`, `default_simulator`, `arbiter`, `proser`).

## Runtime concept in one page

1. Player submits input.
2. Engine records input as an event.
3. Intent and loremaster stages interpret/assess the action in structured form.
4. Simulator modules produce structured proposals (`ProposedDiff`).
5. Arbiter commits final diff (`CommittedDiff`), and engine records warnings + persists snapshots.
6. Narration is generated from committed outcomes.
7. Events/snapshots are persisted to per-run `world_state.db`, and frontend refreshes via `GET /run/:runId/state`.
8. Optional step mode executes one router stage at a time via `/turn/step/start` + `/turn/step/next`.

The current implementation is intentionally minimal and deterministic in places, but the structure is built to grow into a full modular simulation pipeline.

## Key API surface (current)

- `GET /`
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

## Where to read next

- `README.md` for quickstart and local setup.
- `docs/Architecture.md` for deeper system design.
- `docs/Conventions.md` for coding, naming, and API conventions.
