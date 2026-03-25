import path from "node:path";
import { mkdir, readFile, readdir, stat } from "node:fs/promises";
import sqlite3 from "sqlite3";
import { open } from "sqlite";
import {
  NarrativeCapsuleSchema,
  type NarrativeCapsule,
  type PipelineStepEvent,
  type TurnExecutionState,
} from "@morpheus/shared";

type SessionDb = Awaited<ReturnType<typeof open>>;

const NARRATIVE_CAPSULE_META_KEY = "narrative_capsule";

export interface SessionStateResult {
  messages: Array<{
    turn: number;
    playerText: string;
    engineText: string;
  }>;
  debugEntries: Array<{
    timestamp: string;
    turn: number;
    trace: unknown;
  }>;
  nextTurn: number;
}

export interface PipelineStateResult {
  runId: string;
  turn: number;
  execution: TurnExecutionState | null;
  events: PipelineStepEvent[];
}

export interface SnapshotRow {
  turn: number;
  worldState: unknown;
  viewState: unknown;
}

interface RunLocation {
  runId: string;
  gameProjectId: string;
  sessionDir: string;
  dbPath: string;
}

const runLocationCache = new Map<string, RunLocation>();

function resolveGameProjectsRoot(): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects");
}

function resolveSavedRootDir(gameProjectId: string): string {
  return path.join(resolveGameProjectsRoot(), gameProjectId, "saved");
}

export function getSessionDirPath(gameProjectId: string, runId: string): string {
  return path.join(resolveSavedRootDir(gameProjectId), runId);
}

export function getRunDbPath(gameProjectId: string, runId: string): string {
  return path.join(getSessionDirPath(gameProjectId, runId), "world_state.db");
}

async function openSessionDb(dbPath: string) {
  const db = await open({
    filename: dbPath,
    driver: sqlite3.Database,
  });

  await db.exec(`PRAGMA journal_mode = WAL;`);
  return db;
}

async function initializeSessionSchema(db: SessionDb): Promise<void> {
  await db.exec(`
    CREATE TABLE IF NOT EXISTS meta (
      key TEXT PRIMARY KEY,
      value TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      turn INTEGER NOT NULL,
      event_type TEXT NOT NULL,
      payload TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS snapshots (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      turn INTEGER NOT NULL,
      world_state TEXT NOT NULL,
      view_state TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS lore (
      subject TEXT PRIMARY KEY,
      data TEXT NOT NULL,
      source TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS turn_execution (
      run_id TEXT NOT NULL,
      turn INTEGER NOT NULL,
      mode TEXT NOT NULL,
      cursor INTEGER NOT NULL DEFAULT 0,
      completed INTEGER NOT NULL DEFAULT 0,
      player_input TEXT NOT NULL,
      player_id TEXT NOT NULL,
      request_id TEXT NOT NULL,
      game_project_id TEXT NOT NULL,
      checkpoint TEXT NOT NULL DEFAULT '{}',
      result TEXT NOT NULL DEFAULT '{}',
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
      PRIMARY KEY (run_id, turn)
    );

    CREATE TABLE IF NOT EXISTS pipeline_events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      run_id TEXT NOT NULL,
      turn INTEGER NOT NULL,
      step_number INTEGER NOT NULL,
      payload TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
  `);
}

function parseCsvLine(line: string): string[] {
  const values: string[] = [];
  let current = "";
  let inQuotes = false;
  for (let i = 0; i < line.length; i += 1) {
    const ch = line[i];
    if (ch === "\"") {
      if (inQuotes && line[i + 1] === "\"") {
        current += "\"";
        i += 1;
      } else {
        inQuotes = !inQuotes;
      }
      continue;
    }
    if (ch === "," && !inQuotes) {
      values.push(current.trim());
      current = "";
      continue;
    }
    current += ch;
  }
  values.push(current.trim());
  return values.map((value) => value.replace(/^"(.*)"$/, "$1").trim());
}

function parseSimpleFactionYaml(raw: string): Array<Record<string, string>> {
  const lines = raw.split(/\r?\n/);
  const items: Array<Record<string, string>> = [];
  let current: Record<string, string> | null = null;

  for (const originalLine of lines) {
    const line = originalLine.trimEnd();
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#") || trimmed === "factions:") continue;

    if (/^-\s+/.test(trimmed)) {
      if (current && Object.keys(current).length > 0) items.push(current);
      current = {};
      const rest = trimmed.replace(/^-\s+/, "");
      const keyValue = rest.match(/^([^:]+):\s*(.+)$/);
      if (keyValue) {
        current[keyValue[1].trim()] = keyValue[2].trim();
      }
      continue;
    }

    const keyValue = trimmed.match(/^([^:]+):\s*(.+)$/);
    if (keyValue && current) {
      current[keyValue[1].trim()] = keyValue[2].trim();
    }
  }

  if (current && Object.keys(current).length > 0) items.push(current);
  return items;
}

async function seedLoreTable(db: SessionDb, gameProjectId: string): Promise<void> {
  const loreDir = path.join(resolveGameProjectsRoot(), gameProjectId, "lore");
  const worldPath = path.join(loreDir, "world.md");
  const csvPath = path.join(loreDir, "default_lore_entries.csv");
  const factionsYamlPath = path.join(resolveGameProjectsRoot(), gameProjectId, "tables", "factions.yaml");

  try {
    const worldRaw = (await readFile(worldPath, "utf8")).trim();
    if (worldRaw.length > 0) {
      await db.run(
        `INSERT OR REPLACE INTO lore (subject, data, source) VALUES (?, ?, ?)`,
        "world_context",
        worldRaw,
        "lore/world.md",
      );
    }
  } catch {
    // Optional world.md seed; ignore when absent.
  }

  try {
    const csvRaw = await readFile(csvPath, "utf8");
    const lines = csvRaw
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line.length > 0 && !line.startsWith("#"));
    if (lines.length === 0) return;

    const headers = parseCsvLine(lines[0]).map((header) => header.toLowerCase());
    const subjectIndex = headers.indexOf("subject");
    const dataIndex = headers.findIndex(
      (header) => header === "data" || header === "description" || header === "entry",
    );
    if (subjectIndex < 0 || dataIndex < 0) return;

    for (const line of lines.slice(1)) {
      const columns = parseCsvLine(line);
      const subject = columns[subjectIndex]?.trim();
      const data = columns[dataIndex]?.trim();
      if (!subject || !data) continue;
      await db.run(
        `INSERT OR REPLACE INTO lore (subject, data, source) VALUES (?, ?, ?)`,
        subject,
        data,
        "lore/default_lore_entries.csv",
      );
    }
  } catch {
    // Optional CSV seed; ignore when absent.
  }

  try {
    const factionsYaml = await readFile(factionsYamlPath, "utf8");
    const entries = parseSimpleFactionYaml(factionsYaml);
    for (const entry of entries) {
      const subject = entry.id?.trim();
      if (!subject) continue;
      const relation = entry.relationToPlayer?.trim();
      const name = entry.name?.trim();
      const fragments = [
        name ? `name=${name}` : null,
        relation ? `relationToPlayer=${relation}` : null,
      ].filter((value): value is string => value !== null);
      if (fragments.length === 0) continue;
      await db.run(
        `INSERT OR REPLACE INTO lore (subject, data, source) VALUES (?, ?, ?)`,
        subject,
        fragments.join("; "),
        "tables/factions.yaml",
      );
    }
  } catch {
    // Optional factions.yaml seed; ignore when absent.
  }
}

export async function initializeRunStore(gameProjectId: string, runId: string): Promise<void> {
  await mkdir(getSessionDirPath(gameProjectId, runId), { recursive: true });
  const dbPath = getRunDbPath(gameProjectId, runId);
  const db = await openSessionDb(dbPath);
  try {
    await initializeSessionSchema(db);
    await db.run(`INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)`, "run_id", runId);
    await db.run(
      `INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)`,
      "game_project_id",
      gameProjectId,
    );
    await db.run(`INSERT OR IGNORE INTO snapshots (turn, world_state, view_state) VALUES (?, ?, ?)`, 0, JSON.stringify({
      gameProjectId,
      entities: [],
      facts: [],
      anchors: [],
    }), JSON.stringify({ player: { observations: [] } }));
    await seedLoreTable(db, gameProjectId);
    runLocationCache.set(runId, {
      runId,
      gameProjectId,
      sessionDir: getSessionDirPath(gameProjectId, runId),
      dbPath,
    });
  } finally {
    await db.close();
  }
}

export async function openRunStore(gameProjectId: string, runId: string): Promise<SessionDb> {
  const db = await openSessionDb(getRunDbPath(gameProjectId, runId));
  await initializeSessionSchema(db);
  return db;
}

export async function listSessionsOnDisk(
  gameProjectId: string,
): Promise<Array<{ sessionId: string; createdAt: string }>> {
  const savedRootDir = resolveSavedRootDir(gameProjectId);
  try {
    const entries = await readdir(savedRootDir, { withFileTypes: true });
    const sessions = await Promise.all(
      entries
        .filter((entry) => entry.isDirectory())
        .map(async (entry) => {
          const sessionDir = path.join(savedRootDir, entry.name);
          const dbPath = path.join(sessionDir, "world_state.db");
          try {
            const dbStats = await stat(dbPath);
            return {
              sessionId: entry.name,
              createdAt: dbStats.birthtime.toISOString(),
              sortTime: dbStats.birthtimeMs,
            };
          } catch {
            return null;
          }
        }),
    );

    return sessions
      .filter((session): session is { sessionId: string; createdAt: string; sortTime: number } =>
        session !== null,
      )
      .sort((a, b) => b.sortTime - a.sortTime)
      .map(({ sessionId, createdAt }) => ({ sessionId, createdAt }));
  } catch {
    return [];
  }
}

export async function resolveRunLocation(runId: string): Promise<RunLocation | null> {
  const cached = runLocationCache.get(runId);
  if (cached) {
    try {
      const dbStats = await stat(cached.dbPath);
      if (dbStats.isFile()) {
        return cached;
      }
      runLocationCache.delete(runId);
    } catch {
      runLocationCache.delete(runId);
    }
  }

  const gameProjectsRoot = resolveGameProjectsRoot();
  let gameProjects: Array<{ name: string }> = [];
  try {
    const entries = await readdir(gameProjectsRoot, { withFileTypes: true });
    gameProjects = entries.filter((entry) => entry.isDirectory()).map((entry) => ({ name: entry.name }));
  } catch {
    return null;
  }

  for (const gameProject of gameProjects) {
    const sessionDir = getSessionDirPath(gameProject.name, runId);
    const dbPath = path.join(sessionDir, "world_state.db");
    try {
      const dbStats = await stat(dbPath);
      if (!dbStats.isFile()) continue;
      const resolved: RunLocation = {
        runId,
        gameProjectId: gameProject.name,
        sessionDir,
        dbPath,
      };
      runLocationCache.set(runId, resolved);
      return resolved;
    } catch {
      // Continue searching in other game projects.
    }
  }

  return null;
}

export async function readSessionState(gameProjectId: string, runId: string): Promise<SessionStateResult> {
  const db = await openRunStore(gameProjectId, runId);
  try {
    const rows = await db.all<Array<{
      turn: number;
      event_type: string;
      payload: string;
      created_at: string;
    }>>(`SELECT turn, event_type, payload, created_at FROM events ORDER BY turn ASC, id ASC`);

    const turns = new Map<number, { playerText: string; engineText: string; trace: unknown; timestamp: string }>();
    for (const row of rows) {
      let payload: unknown = null;
      try {
        payload = JSON.parse(row.payload);
      } catch {
        payload = null;
      }

      const entry = turns.get(row.turn) ?? {
        playerText: "",
        engineText: "",
        trace: null,
        timestamp: row.created_at,
      };
      entry.timestamp = row.created_at;

      if (row.event_type === "player_input" && payload && typeof payload === "object") {
        const text = (payload as { text?: unknown }).text;
        if (typeof text === "string") entry.playerText = text;
      }

      if (row.event_type === "module_trace" && payload && typeof payload === "object") {
        entry.trace = payload;
        const narrationText = (payload as { narrationText?: unknown }).narrationText;
        if (typeof narrationText === "string") entry.engineText = narrationText;
      }

      turns.set(row.turn, entry);
    }

    const orderedTurns = [...turns.entries()].sort((a, b) => a[0] - b[0]);
    const messages = orderedTurns.map(([turn, data]) => ({
      turn,
      playerText: data.playerText,
      engineText: data.engineText,
    }));
    const debugEntries = orderedTurns.map(([turn, data]) => ({
      timestamp: data.timestamp,
      turn,
      trace: data.trace,
    }));

    const maxTurnRow = await db.get<{ maxTurn: number | null }>(
      `SELECT MAX(turn) as maxTurn FROM snapshots`,
    );

    return {
      messages,
      debugEntries,
      nextTurn: (maxTurnRow?.maxTurn ?? 0) + 1,
    };
  } finally {
    await db.close();
  }
}

export async function readLatestSnapshot(db: SessionDb): Promise<SnapshotRow> {
  const row = await db.get<{ turn: number; world_state: string; view_state: string }>(
    `SELECT turn, world_state, view_state FROM snapshots ORDER BY turn DESC, id DESC LIMIT 1`,
  );
  if (!row) {
    return {
      turn: 0,
      worldState: {},
      viewState: {},
    };
  }

  let worldState: unknown = {};
  let viewState: unknown = {};
  try {
    worldState = JSON.parse(row.world_state);
  } catch {
    worldState = {};
  }
  try {
    viewState = JSON.parse(row.view_state);
  } catch {
    viewState = {};
  }
  return {
    turn: row.turn,
    worldState,
    viewState,
  };
}

function asIsoString(value: unknown): string {
  if (typeof value === "string" && value.length > 0) return value;
  return new Date().toISOString();
}

function parseTurnExecutionRow(row: {
  run_id: string;
  turn: number;
  mode: string;
  cursor: number;
  completed: number;
  player_input: string;
  player_id: string;
  request_id: string;
  game_project_id: string;
  result: string;
  created_at: string;
  updated_at: string;
}): TurnExecutionState {
  let result: { narrationText?: string; warnings?: string[] } = {};
  try {
    result = JSON.parse(row.result) as { narrationText?: string; warnings?: string[] };
  } catch {
    result = {};
  }
  return {
    runId: row.run_id,
    turn: row.turn,
    mode: row.mode === "step" ? "step" : "normal",
    cursor: row.cursor,
    completed: row.completed === 1,
    createdAt: asIsoString(row.created_at),
    updatedAt: asIsoString(row.updated_at),
    playerInput: row.player_input,
    playerId: row.player_id,
    requestId: row.request_id,
    gameProjectId: row.game_project_id,
    result: {
      narrationText: typeof result.narrationText === "string" ? result.narrationText : undefined,
      warnings: Array.isArray(result.warnings)
        ? result.warnings.filter((warning): warning is string => typeof warning === "string")
        : [],
    },
  };
}

export async function getTurnExecutionState(
  db: SessionDb,
  runId: string,
  turn: number,
): Promise<TurnExecutionState | null> {
  const row = await db.get<{
    run_id: string;
    turn: number;
    mode: string;
    cursor: number;
    completed: number;
    player_input: string;
    player_id: string;
    request_id: string;
    game_project_id: string;
    result: string;
    created_at: string;
    updated_at: string;
  }>(
    `SELECT run_id, turn, mode, cursor, completed, player_input, player_id, request_id, game_project_id, result, created_at, updated_at
     FROM turn_execution
     WHERE run_id = ? AND turn = ?`,
    runId,
    turn,
  );
  if (!row) return null;
  return parseTurnExecutionRow(row);
}

export async function createTurnExecution(
  db: SessionDb,
  state: {
    runId: string;
    turn: number;
    mode: "normal" | "step";
    playerInput: string;
    playerId: string;
    requestId: string;
    gameProjectId: string;
    checkpoint: unknown;
  },
): Promise<TurnExecutionState> {
  await db.run(
    `INSERT INTO turn_execution
      (run_id, turn, mode, cursor, completed, player_input, player_id, request_id, game_project_id, checkpoint, result)
     VALUES (?, ?, ?, 0, 0, ?, ?, ?, ?, ?, ?)`,
    state.runId,
    state.turn,
    state.mode,
    state.playerInput,
    state.playerId,
    state.requestId,
    state.gameProjectId,
    JSON.stringify(state.checkpoint ?? {}),
    JSON.stringify({ warnings: [] }),
  );
  const created = await getTurnExecutionState(db, state.runId, state.turn);
  if (!created) {
    throw new Error("Failed to create turn execution state.");
  }
  return created;
}

export async function readTurnExecutionCheckpoint(
  db: SessionDb,
  runId: string,
  turn: number,
): Promise<unknown> {
  const row = await db.get<{ checkpoint: string }>(
    `SELECT checkpoint FROM turn_execution WHERE run_id = ? AND turn = ?`,
    runId,
    turn,
  );
  if (!row) return null;
  try {
    return JSON.parse(row.checkpoint);
  } catch {
    return null;
  }
}

export async function updateTurnExecutionProgress(
  db: SessionDb,
  state: {
    runId: string;
    turn: number;
    cursor: number;
    checkpoint: unknown;
    completed?: boolean;
    narrationText?: string;
    warnings?: string[];
  },
): Promise<TurnExecutionState> {
  const nextResult = {
    ...(state.narrationText === undefined ? {} : { narrationText: state.narrationText }),
    warnings: state.warnings ?? [],
  };
  await db.run(
    `UPDATE turn_execution
     SET cursor = ?, checkpoint = ?, completed = ?, result = ?, updated_at = CURRENT_TIMESTAMP
     WHERE run_id = ? AND turn = ?`,
    state.cursor,
    JSON.stringify(state.checkpoint ?? {}),
    state.completed ? 1 : 0,
    JSON.stringify(nextResult),
    state.runId,
    state.turn,
  );
  const updated = await getTurnExecutionState(db, state.runId, state.turn);
  if (!updated) {
    throw new Error("Failed to update turn execution state.");
  }
  return updated;
}

export async function appendPipelineEvent(
  db: SessionDb,
  runId: string,
  turn: number,
  event: PipelineStepEvent,
): Promise<void> {
  await db.run(
    `INSERT INTO pipeline_events (run_id, turn, step_number, payload) VALUES (?, ?, ?, ?)`,
    runId,
    turn,
    event.stepNumber,
    JSON.stringify(event),
  );
}

export async function listPipelineEvents(
  db: SessionDb,
  runId: string,
  turn: number,
): Promise<PipelineStepEvent[]> {
  const rows = await db.all<Array<{ payload: string }>>(
    `SELECT payload FROM pipeline_events WHERE run_id = ? AND turn = ? ORDER BY step_number ASC, id ASC`,
    runId,
    turn,
  );
  const events: PipelineStepEvent[] = [];
  for (const row of rows) {
    try {
      const parsed = JSON.parse(row.payload) as PipelineStepEvent;
      events.push(parsed);
    } catch {
      // Ignore malformed rows.
    }
  }
  return events;
}

export async function readPipelineState(
  gameProjectId: string,
  runId: string,
  turn: number,
): Promise<PipelineStateResult> {
  const db = await openRunStore(gameProjectId, runId);
  try {
    const execution = await getTurnExecutionState(db, runId, turn);
    const events = await listPipelineEvents(db, runId, turn);
    return {
      runId,
      turn,
      execution,
      events,
    };
  } finally {
    await db.close();
  }
}

export async function readLoreRowsBySubjects(
  db: SessionDb,
  subjects: string[],
): Promise<Array<{ subject: string; data: string; source: string }>> {
  const unique = [...new Set(subjects.map((s) => s.trim()).filter((s) => s.length > 0))];
  if (unique.length === 0) return [];
  const placeholders = unique.map(() => "?").join(", ");
  const rows = await db.all<Array<{ subject: string; data: string; source: string }>>(
    `SELECT subject, data, source FROM lore WHERE subject IN (${placeholders})`,
    ...unique,
  );
  return rows.filter(
    (row) =>
      typeof row.subject === "string" &&
      row.subject.trim().length > 0 &&
      typeof row.data === "string" &&
      row.data.trim().length > 0 &&
      typeof row.source === "string" &&
      row.source.trim().length > 0,
  );
}

export async function listAllLoreRows(
  db: SessionDb,
): Promise<Array<{ subject: string; data: string; source: string }>> {
  const rows = await db.all<Array<{ subject: string; data: string; source: string }>>(
    `SELECT subject, data, source FROM lore`,
  );
  return rows.filter(
    (row) =>
      typeof row.subject === "string" &&
      row.subject.trim().length > 0 &&
      typeof row.data === "string" &&
      row.data.trim().length > 0,
  );
}

export async function readNarrativeCapsule(db: SessionDb): Promise<NarrativeCapsule | null> {
  const row = await db.get<{ value: string }>(
    `SELECT value FROM meta WHERE key = ?`,
    NARRATIVE_CAPSULE_META_KEY,
  );
  if (!row?.value) return null;
  try {
    return NarrativeCapsuleSchema.parse(JSON.parse(row.value));
  } catch {
    return null;
  }
}

export async function writeNarrativeCapsule(db: SessionDb, capsule: NarrativeCapsule): Promise<void> {
  const parsed = NarrativeCapsuleSchema.parse(capsule);
  await db.run(`INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)`, NARRATIVE_CAPSULE_META_KEY, JSON.stringify(parsed));
}
