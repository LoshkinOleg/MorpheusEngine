# LLM_FrontEnd

## Purpose

Track frontend contracts for session lifecycle, chat turn submission, and DB-backed Game/Debug UI behavior.

## Last verified

2026-02-26

## Owner / Source of truth

- UI logic: `apps/frontend/src/App.tsx`
- API client: `apps/frontend/src/api.ts`
- Layout/styling: `apps/frontend/src/styles.css`

## Invariants / Contracts (Facts)

- Frontend talks to backend using `VITE_API_BASE_URL`.
- Session lifecycle:
  - Start with `POST /run/start`
  - Send turns via `POST /turn`
  - Optional stepped turns via `POST /turn/step/start` + `POST /turn/step/next`
  - Refresh display from `GET /run/:runId/state`
- Pipeline timeline reads from `GET /run/:runId/turn/:turn/pipeline` and/or persisted `module_trace.pipelineEvents`.
- Session selector options come from `GET /game_projects/:id/sessions`.
- Game UI derives from persisted `messages[]` returned by backend run-state projection.
- Debug UI derives from persisted `debugEntries[]` (module trace payloads) and supports per-turn selection.
- Debug UI supports ordered pipeline timeline cards and step controls (start/next/run-to-end).
- Timeline order includes explicit `arbiter` module event before `proser`, with final `world_state_update` persistence step.
- Debug trace now surfaces explicit refusal details when backend rejects invalid actions.
- Proser narration should align more tightly with committed operations as backend grounding enforcement improves.
- Model conversations are rendered with readable module/attempt formatting.

## Key entry points

- App state + effects: `apps/frontend/src/App.tsx`
- HTTP wrappers and typed payloads: `apps/frontend/src/api.ts`
- Panel/card layout rules: `apps/frontend/src/styles.css`

## Operational workflows

- Local frontend run:
  - [ ] Set `VITE_API_BASE_URL` to backend URL
  - [ ] Run `npm run dev:frontend`
  - [ ] Start/choose session and submit turn
  - [ ] Confirm Game/Debug panels refresh from `GET /run/:runId/state`

## Gotchas / footguns

- Do not reintroduce synthetic local placeholders that diverge from backend state projection.
- Blank/whitespace player input is valid and should advance turn; avoid re-adding strict trim-blocking.
- Very large prompt/response payloads can hurt readability if debug collapse/scroll rules are removed.

## Recent changes

- Switched from log-file-backed UI loading to DB-backed run-state loading.
- Upgraded model conversation rendering to structured module/attempt cards.
- Kept split Game UI (top) + Debug UI (bottom) with turn navigation.
- Preserved manual Debug turn selection during polling (older turn view no longer snaps back to latest turn).
- Added refusal visibility in Debug cards when backend records invalid-action refusal reason.
- Clarification-specific debug semantics were removed; UI now treats invalid actions as binary refusal with justification.

## To verify

- Add request-level loading diagnostics when state polling races with turn submission.
- Add provenance/citation panel for retrieved lore evidence in debug trace.

## References / Links

- `docs/ImplementationStatus.md`
- `docs/DirectionGuardrails.md`
