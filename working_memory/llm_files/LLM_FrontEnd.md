# LLM_FrontEnd

## Purpose

Track frontend contracts for session lifecycle, chat turn submission, and file-backed Game/Debug UI behavior.

## Last verified

2026-02-25

## Owner / Source of truth

- UI logic: `apps/frontend/src/App.tsx`
- API client: `apps/frontend/src/api.ts`
- Layout/styling: `apps/frontend/src/styles.css`

## Invariants / Contracts (Facts)

- Frontend talks to backend using `VITE_API_BASE_URL`.
  - Evidence: `apps/frontend/src/api.ts`, `apps/frontend/.env.example`
- Session lifecycle:
  - Start with `POST /run/start`
  - Send turns via `POST /turn`
  - Refresh display from `GET /run/:runId/logs`
  - Evidence: `apps/frontend/src/App.tsx`, `apps/frontend/src/api.ts`
- Session selector options come from disk-backed backend endpoint (`GET /game_projects/:id/sessions`).
  - Evidence: `apps/frontend/src/api.ts`, `apps/frontend/src/App.tsx`
- Player-facing Game UI derives from `prose.log` entries, not synthetic local placeholders.
  - Evidence: `apps/frontend/src/App.tsx` rendering logic
- Debug UI derives from `debug.log` entries and supports per-turn selection with latest-turn auto-scoping.
  - Evidence: `apps/frontend/src/App.tsx`
- Full provider conversations are displayed in Debug UI (`intent_extractor`, `loremaster`, `default_simulator`, `proser`).
  - Evidence: `apps/frontend/src/App.tsx` debug card sections

## Key entry points

- App state + effects: `apps/frontend/src/App.tsx`
- HTTP wrappers and typed payloads: `apps/frontend/src/api.ts`
- Panel/card layout rules: `apps/frontend/src/styles.css`

## Operational workflows

- Local frontend run:
  - [ ] Set `VITE_API_BASE_URL` to backend URL
  - [ ] Run `npm run dev:frontend`
  - [ ] Start/choose session and submit turn
  - [ ] Confirm Game/Debug panels refresh from logs
- Debug trace inspection:
  - [ ] Submit at least two turns
  - [ ] Use turn selector arrows
  - [ ] Confirm selected turn trace sections match `debug.log`

## Gotchas / footguns

- Do not reintroduce synthetic "welcome" log entries; this causes file/view mismatch.
- Blank/whitespace player input is valid and should advance turn; avoid re-adding strict trim-blocking.
- Large debug JSON can hurt readability/performance if layout constraints are removed.

## Recent changes

- UI split to vertical Game UI (top) + Debug UI (bottom).
- Added turn-focused Debug UI cards and per-module conversation views.
- Added "open saved folder" action for selected session.
- Updated turn payload handling to show `warnings` (without validator module response object).

## To verify

- Add richer debug trace UI (collapsible attempt/message blocks) without breaking raw data visibility.
- Add request-level loading diagnostics when log polling races with turn submission.

## References / Links

- `docs/ImplementationStatus.md`
- `docs/DirectionGuardrails.md`
