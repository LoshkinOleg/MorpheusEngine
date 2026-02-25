import path from "node:path";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";

export interface SessionProseEntry {
  turn: number;
  playerText: string;
  engineText: string;
}

export interface SessionLogsResult {
  proseEntries: SessionProseEntry[];
  debugEntries: Array<{ timestamp: string; turn: number; trace: unknown }>;
  nextTurn: number;
}

function resolveSessionDir(gameProjectId: string, sessionId: string): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects", gameProjectId, "saved", sessionId);
}

export function getSessionDirPath(gameProjectId: string, sessionId: string): string {
  return resolveSessionDir(gameProjectId, sessionId);
}

function resolveSavedRootDir(gameProjectId: string): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects", gameProjectId, "saved");
}

async function readUtf8OrEmpty(filePath: string): Promise<string> {
  try {
    return await readFile(filePath, "utf8");
  } catch {
    return "";
  }
}

function parseProseEntries(proseLog: string): SessionProseEntry[] {
  const entries: SessionProseEntry[] = [];
  const regex = /--- turn (\d+) ---\nYou: ([\s\S]*?)\nEngine: ([\s\S]*?)\n\n/g;

  for (const match of proseLog.matchAll(regex)) {
    const turn = Number(match[1]);
    const playerText = match[2].replace(/\\n/g, "\n");
    const engineText = match[3].replace(/\\n/g, "\n");
    entries.push({ turn, playerText, engineText });
  }

  entries.sort((a, b) => a.turn - b.turn);
  return entries;
}

function parseDebugEntries(debugLog: string) {
  const entries: Array<{ timestamp: string; turn: number; trace: unknown }> = [];
  const lines = debugLog.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);

  for (const line of lines) {
    try {
      const parsed = JSON.parse(line) as {
        timestamp?: string;
        turn?: number;
        trace?: unknown;
      };
      const turn = parsed.turn;
      if (typeof turn !== "number" || !Number.isInteger(turn)) continue;
      entries.push({
        timestamp: parsed.timestamp ?? "",
        turn,
        trace: parsed.trace,
      });
    } catch {
      // Ignore malformed lines to keep logs resilient.
    }
  }

  entries.sort((a, b) => a.turn - b.turn);
  return entries;
}

export async function initializeSessionLogs(gameProjectId: string, sessionId: string): Promise<void> {
  const sessionDir = resolveSessionDir(gameProjectId, sessionId);
  await mkdir(sessionDir, { recursive: true });
  await writeFile(path.join(sessionDir, "prose.log"), "", { encoding: "utf8", flag: "w" });
  await writeFile(path.join(sessionDir, "debug.log"), "", { encoding: "utf8", flag: "w" });
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
          const stats = await stat(sessionDir);
          return {
            sessionId: entry.name,
            createdAt: stats.birthtime.toISOString(),
            sortTime: stats.birthtimeMs,
          };
        }),
    );

    sessions.sort((a, b) => b.sortTime - a.sortTime);
    return sessions.map(({ sessionId, createdAt }) => ({ sessionId, createdAt }));
  } catch {
    return [];
  }
}

export async function readSessionLogs(gameProjectId: string, sessionId: string): Promise<SessionLogsResult> {
  const sessionDir = resolveSessionDir(gameProjectId, sessionId);
  const proseRaw = await readUtf8OrEmpty(path.join(sessionDir, "prose.log"));
  const debugRaw = await readUtf8OrEmpty(path.join(sessionDir, "debug.log"));

  const proseEntries = parseProseEntries(proseRaw);
  const debugEntries = parseDebugEntries(debugRaw);
  const highestTurn = Math.max(
    0,
    ...proseEntries.map((entry) => entry.turn),
    ...debugEntries.map((entry) => entry.turn),
  );

  return {
    proseEntries,
    debugEntries,
    nextTurn: highestTurn + 1,
  };
}
