import "dotenv/config";
import express from "express";
import type { Response } from "express";
import cors from "cors";
import { randomUUID } from "node:crypto";
import { mkdir } from "node:fs/promises";
import { spawn } from "node:child_process";
import { initDb } from "./db.js";
import { processTurn } from "./engine.js";
import { loadGameProjectManifest } from "./gameProject.js";
import {
  getSessionDirPath,
  initializeSessionLogs,
  listSessionsOnDisk,
  readSessionLogs,
} from "./logs.js";
import { createLlmProviderFromEnv } from "./providers/factory.js";

const port = Number(process.env.PORT ?? 8787);
const defaultGameProjectId = process.env.GAME_PROJECT_ID ?? "sandcrawler";

function sendError(
  res: Response,
  status: number,
  code: string,
  message: string,
  requestId: string,
  details?: unknown,
) {
  res.status(status).json({
    error: {
      code,
      message,
      requestId,
      ...(details === undefined ? {} : { details }),
    },
  });
}

function logInfo(eventName: string, details: Record<string, string | number>) {
  const parts = Object.entries(details).map(([key, value]) => `${key}=${value}`);
  console.log(`morpheus event=${eventName} ${parts.join(" ")}`);
}

function openFolderInExplorer(folderPath: string) {
  if (process.platform === "win32") {
    const child = spawn("explorer", [folderPath], {
      detached: true,
      stdio: "ignore",
    });
    child.unref();
    return;
  }

  if (process.platform === "darwin") {
    const child = spawn("open", [folderPath], {
      detached: true,
      stdio: "ignore",
    });
    child.unref();
    return;
  }

  const child = spawn("xdg-open", [folderPath], {
    detached: true,
    stdio: "ignore",
  });
  child.unref();
}

async function main() {
  const llmProvider = createLlmProviderFromEnv(process.env);
  const db = await initDb();
  const app = express();
  app.use(cors());
  app.use(express.json());

  app.get("/health", (_req, res) => {
    res.json({ ok: true });
  });

  app.get("/", (_req, res) => {
    res.json({
      ok: true,
      service: "morpheus-backend",
      message: "Backend is running. Open frontend at http://localhost:5173",
    });
  });

  app.get("/game_projects/:id", (req, res) => {
    const requestId = randomUUID();
    try {
      const manifest = loadGameProjectManifest(req.params.id);
      res.json(manifest);
    } catch (error) {
      sendError(
        res,
        404,
        "GAME_PROJECT_NOT_FOUND",
        "Game project not found.",
        requestId,
        String(error),
      );
    }
  });

  app.get("/game_projects/:id/sessions", async (req, res) => {
    const requestId = randomUUID();
    const gameProjectId = req.params.id;

    try {
      const sessions = await listSessionsOnDisk(gameProjectId);
      res.json({
        gameProjectId,
        sessions,
      });
    } catch (error) {
      sendError(
        res,
        500,
        "SESSION_LIST_FAILED",
        "Could not list sessions.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.post("/run/start", async (_req, res) => {
    const requestId = randomUUID();
    try {
      const runId = randomUUID();
      const manifest = loadGameProjectManifest(defaultGameProjectId);

      await db.run(`INSERT INTO runs (id, game_project_id) VALUES (?, ?)`, runId, manifest.id);
      await db.run(
        `INSERT INTO snapshots (run_id, turn, world_state, view_state) VALUES (?, ?, ?, ?)`,
        runId,
        0,
        JSON.stringify({ gameProjectId: manifest.id, entities: [], facts: [], anchors: [] }),
        JSON.stringify({ player: { observations: [] } }),
      );
      await initializeSessionLogs(manifest.id, runId);

      logInfo("run_started", {
        request_id: requestId,
        run_id: runId,
        game_project: manifest.id,
      });
      res.json({ runId, gameProject: manifest.id });
    } catch (error) {
      sendError(
        res,
        500,
        "RUN_START_FAILED",
        "Could not start run.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.get("/run/:runId/logs", async (req, res) => {
    const requestId = randomUUID();
    const runId = req.params.runId;

    try {
      const existingRun = await db.get<{ id: string; game_project_id: string }>(
        `SELECT id, game_project_id FROM runs WHERE id = ?`,
        runId,
      );
      if (!existingRun) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }

      const logs = await readSessionLogs(existingRun.game_project_id, runId);
      res.json({
        runId,
        gameProjectId: existingRun.game_project_id,
        ...logs,
      });
    } catch (error) {
      sendError(
        res,
        500,
        "RUN_LOGS_READ_FAILED",
        "Could not read run logs.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.post("/run/:runId/open-saved-folder", async (req, res) => {
    const requestId = randomUUID();
    const runId = req.params.runId;

    try {
      const existingRun = await db.get<{ id: string; game_project_id: string }>(
        `SELECT id, game_project_id FROM runs WHERE id = ?`,
        runId,
      );
      if (!existingRun) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }

      const sessionDir = getSessionDirPath(existingRun.game_project_id, runId);
      await mkdir(sessionDir, { recursive: true });
      openFolderInExplorer(sessionDir);
      res.json({ ok: true, runId, openedPath: sessionDir });
    } catch (error) {
      sendError(
        res,
        500,
        "OPEN_SAVED_FOLDER_FAILED",
        "Could not open session saved folder.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.post("/turn", async (req, res) => {
    const requestId = randomUUID();
    const { runId, turn, playerInput, playerId } = req.body as {
      runId: string;
      turn: number;
      playerInput: string;
      playerId: string;
    };

    if (!runId || typeof playerInput !== "string" || !playerId || !Number.isInteger(turn)) {
      sendError(
        res,
        400,
        "BAD_TURN_REQUEST",
        "Missing or invalid turn payload fields.",
        requestId,
        "Expected runId, playerInput, playerId, and integer turn.",
      );
      return;
    }

    if (turn < 1) {
      sendError(res, 400, "INVALID_TURN_INDEX", "Turn must be >= 1.", requestId);
      return;
    }

    try {
      const existingRun = await db.get<{ id: string; game_project_id: string }>(
        `SELECT id, game_project_id FROM runs WHERE id = ?`,
        runId,
      );
      if (!existingRun) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }

      const turnRow = await db.get<{ maxTurn: number | null }>(
        `SELECT MAX(turn) as maxTurn FROM snapshots WHERE run_id = ?`,
        runId,
      );
      const expectedTurn = (turnRow?.maxTurn ?? 0) + 1;

      if (turn !== expectedTurn) {
        sendError(
          res,
          409,
          "TURN_SEQUENCE_CONFLICT",
          "Turn is out of sequence.",
          requestId,
          { expectedTurn, receivedTurn: turn },
        );
        return;
      }

      const result = await processTurn(
        db,
        {
          runId,
          gameProjectId: existingRun.game_project_id,
          turn,
          playerInput,
          playerId,
        },
        llmProvider,
      );

      logInfo("turn_processed", {
        request_id: requestId,
        run_id: runId,
        turn,
        warnings: result.warnings.length,
      });

      res.json(result);
    } catch (error) {
      sendError(
        res,
        500,
        "TURN_PROCESSING_FAILED",
        "Could not process turn.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.listen(port, () => {
    // Intentional single-line startup log for easy tooling parsing.
    console.log(
      `Morpheus backend listening on http://localhost:${port} (llm_provider=${llmProvider.name})`,
    );
  });
}

main().catch((error) => {
  console.error("Backend startup failed:", error);
  process.exit(1);
});
