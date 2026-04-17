# Game projects and session store

Per-game content lives under **`game_projects/<gameProjectId>/`**. The **`gameProjectId`** is a **single path segment** (no slashes, no `..`); it is validated when building filesystem paths for SQLite.

## Layout

```
game_projects/<gameProjectId>/
  manifest.json              # Optional human metadata (id/title); not read by the .NET engine today
  lore/
    default_lore_entries.csv # Seeded into SQLite lore table on /initialize (subject + data columns). Sandcrawler includes general lore plus rows prefixed faction … (former tables/factions.yaml).
  system/
    instructions.md          # Director GM system prompt (required for Director POST /initialize)
  saved/<runId>/
    world_state.db           # Created by session_store POST /initialize
```

The WPF app’s default `gameProjectId` is **`sandcrawler`** (`MainWindow.xaml.cs`). Every `initialize` / `turn` request carries the client-chosen id; **`RunPersistence`** and **`Director`** resolve paths as `game_projects/<gameProjectId>/...` with **no** engine-level fallback to another folder if files are missing (Director fails fast on missing `system/instructions.md` or lore CSV; session store logs a warning if the lore CSV is absent on initialize).

## SQLite (`world_state.db`)

Created by **`RunPersistence.InitializeRun`** when the router forwards **`POST /initialize`** with **`RunStartRequest`**.

Notable tables (see `RunPersistence.InitializeSessionSchema`):

- **`meta`** — key/value (`run_id`, `game_project_id`, …).
- **`events`** — append-only log (`turn`, `event_type`, `payload` JSON).
- **`snapshots`** — one row per completed turn (`turn`, `world_state` JSON, `view_state` JSON).
- **`lore`** — canonical rows (`subject` PK, `data`, `source`); seeded from **`lore/default_lore_entries.csv`** if present when the DB is first created.
- **`turn_execution`**, **`pipeline_events`** — reserved / forward-looking; not on the hot `/turn` path for Director today.

## Lore CSV format

- First non-empty, non-`#` line: header with **`subject`** and **`data`** (aliases **`description`** / **`entry`** accepted for the value column).
- Subsequent lines: CSV rows with quoted fields allowed (same parsing rules as `RunPersistence.ParseCsvLine`).

If the CSV is missing, initialization still succeeds; lore simply stays empty (Director will still require **`system/instructions.md`** and the CSV path to exist for its own prompt assembly — keep files in sync per game project).

## Turn sequencing

- **`/validate_turn`** and **`/persist_turn`** both enforce: next persisted turn must equal **`MAX(snapshots.turn) + 1`**.
- Turn **0** snapshot is inserted at initialize (bootstrap empty world/view JSON).
- Player turns start at **1** from the UI (`_nextTurn` in `MainWindow`).

If the client sends the wrong turn index, **`409`** / **`InvalidOperationException`** paths describe the mismatch (fail fast, no silent repair).

## What gets persisted on `/turn` today

`RunPersistence.PersistTurn`:

- Inserts **`player_input`** event and **`module_trace`** event (the trace tries to interpret `intentResponseBody` as **`IntentResponse`** for readable narration text).
- Copies latest **`world_state`** JSON into the new snapshot (Director path does not yet mutate world state in DB).
- Stores **`view_state`** envelope derived from the Director/intent JSON body.

So the database records **player text + engine response JSON** each turn even though the Director’s **conversation memory** is in-process only.

## Extending toward long-term memory

Future work typically involves:

- Reading recent **`events`** / **`snapshots`** back into the Director (or a dedicated module) on startup.
- Generalizing `persist_turn` payloads beyond the `IntentResponse` shim when tool traces and richer envelopes are needed.

This document only describes the current layout and behavior.
