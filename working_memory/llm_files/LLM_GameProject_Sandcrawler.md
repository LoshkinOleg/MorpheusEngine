# LLM_GameProject_Sandcrawler

## Purpose

Track the sample `sandcrawler` game project structure, session artifacts, and expected usage in local development.

## Last verified

2026-02-25

## Owner / Source of truth

- Game project root: `game_projects/sandcrawler/`
- Manifest and content: `manifest.json`, `lore/`, `tables/`
- Saved sessions: `game_projects/sandcrawler/saved/<session_id>/`

## Invariants / Contracts (Facts)

- Game project metadata is loaded from `manifest.json`.
  - Evidence: `apps/backend/src/gameProject.ts`
- Session artifacts for this game project are created under `saved/<session_id>/`.
  - Evidence: `apps/backend/src/logs.ts` (`initializeSessionLogs`, session directory helpers)
- `prose.log` is player-facing transcript; `debug.log` is per-turn debug payload log.
  - Evidence: `apps/backend/src/engine.ts` log append behavior and frontend consumers in `apps/frontend/src/App.tsx`

## Key entry points

- Manifest loading: `apps/backend/src/gameProject.ts`
- Session file creation/listing: `apps/backend/src/logs.ts`
- Session list endpoint: `apps/backend/src/index.ts` (`GET /game_projects/:id/sessions`)

## Operational workflows

- Create and inspect a session:
  - [ ] Start backend/frontend
  - [ ] Select game project `sandcrawler`
  - [ ] Start a run and submit a turn
  - [ ] Open session folder from frontend button
  - [ ] Inspect `prose.log` and `debug.log`
- Reload behavior:
  - [ ] Refresh frontend page
  - [ ] Confirm session selector can load the same saved run from disk list

## Gotchas / footguns

- Manual edits to logs can change what frontend displays; logs are treated as source of truth.
- Legacy `world-packs` references must not be reintroduced in new code/docs.

## Recent changes

- Session directory model moved from shared per-project logs to per-session log files.
- Session selector switched to disk-based session discovery.
- Removed `validator` module binding from sample manifest for current runtime policy.

## To verify

- Define lifecycle policy for old `saved/<session_id>` data (retention/cleanup).
- Add schema versioning plan for `debug.log` payload evolution.

## References / Links

- `docs/Architecture.md`
- `docs/ImplementationStatus.md`
