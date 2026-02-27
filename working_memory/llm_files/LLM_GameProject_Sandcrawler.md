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
  - Evidence: `apps/backend/src/sessionStore.ts`
- Session authority is `world_state.db` inside each run folder.
  - Evidence: `apps/backend/src/sessionStore.ts` (`getRunDbPath`, schema init)

## Key entry points

- Manifest loading: `apps/backend/src/gameProject.ts`
- Session DB creation/listing: `apps/backend/src/sessionStore.ts`
- Session list endpoint: `apps/backend/src/index.ts` (`GET /game_projects/:id/sessions`)

## Operational workflows

- Create and inspect a session:
  - [ ] Start backend/frontend
  - [ ] Select game project `sandcrawler`
  - [ ] Start a run and submit a turn
  - [ ] Open session folder from frontend button
  - [ ] Inspect `world_state.db`
- Reload behavior:
  - [ ] Refresh frontend page
  - [ ] Confirm session selector can load the same saved run from disk list

## Gotchas / footguns

- Manual DB edits can change what frontend displays; run DB is treated as source of truth.
- Legacy `world-packs` references must not be reintroduced in new code/docs.

## Recent changes

- Session authority moved to per-run SQLite (`world_state.db`) under saved session folders.
- Session selector remains disk-based session discovery (`saved/<session_id>` folders containing DB files).
- Removed `validator` module binding from sample manifest for current runtime policy.

## To verify

- Define lifecycle policy for old `saved/<session_id>` data (retention/cleanup).
- Add schema versioning plan for `events.payload` and `module_trace` payload evolution.

## References / Links

- `docs/Architecture.md`
- `docs/ImplementationStatus.md`
