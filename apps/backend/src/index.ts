import "dotenv/config";
import express from "express";
import type { Response } from "express";
import cors from "cors";
import { randomUUID } from "node:crypto";
import { mkdir } from "node:fs/promises";
import { spawn } from "node:child_process";
import { loadGameProjectManifest } from "./gameProject.js";
import {
  getTurnExecutionState,
  getSessionDirPath,
  initializeRunStore,
  listSessionsOnDisk,
  openRunStore,
  readPipelineState,
  readSessionState,
  resolveRunLocation,
} from "./sessionStore.js";
import {
  advanceTurnStepExecution,
  processTurnViaRouter,
  startTurnStepExecution,
} from "./router/orchestrator.js";

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
      await initializeRunStore(manifest.id, runId);

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

  app.get("/run/:runId/state", async (req, res) => {
    const requestId = randomUUID();
    const runId = req.params.runId;

    try {
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }

      const state = await readSessionState(runLocation.gameProjectId, runId);
      res.json({
        runId,
        gameProjectId: runLocation.gameProjectId,
        ...state,
      });
    } catch (error) {
      sendError(
        res,
        500,
        "RUN_STATE_READ_FAILED",
        "Could not read run state.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.post("/run/:runId/open-saved-folder", async (req, res) => {
    const requestId = randomUUID();
    const runId = req.params.runId;

    try {
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }

      const sessionDir = getSessionDirPath(runLocation.gameProjectId, runId);
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
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }
      const manifest = loadGameProjectManifest(runLocation.gameProjectId);

      const db = await openRunStore(runLocation.gameProjectId, runId);
      let result: Awaited<ReturnType<typeof processTurnViaRouter>>;
      try {
        const turnRow = await db.get<{ maxTurn: number | null }>(`SELECT MAX(turn) as maxTurn FROM snapshots`);
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

        result = await processTurnViaRouter(
          db,
          {
            runId,
            gameProjectId: runLocation.gameProjectId,
            moduleBindings: manifest.moduleBindings,
            turn,
            playerInput,
            playerId,
          },
        );
      } finally {
        await db.close();
      }

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

  app.post("/turn/step/start", async (req, res) => {
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
        "BAD_STEP_START_REQUEST",
        "Missing or invalid step start payload fields.",
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
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }
      const manifest = loadGameProjectManifest(runLocation.gameProjectId);
      const db = await openRunStore(runLocation.gameProjectId, runId);
      try {
        const turnRow = await db.get<{ maxTurn: number | null }>(`SELECT MAX(turn) as maxTurn FROM snapshots`);
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

        const activeExecution = await db.get<{ turn: number }>(
          `SELECT turn FROM turn_execution WHERE run_id = ? AND completed = 0 ORDER BY turn ASC LIMIT 1`,
          runId,
        );
        if (activeExecution && activeExecution.turn !== turn) {
          sendError(
            res,
            409,
            "STEP_EXECUTION_CONFLICT",
            "Another stepped turn is already in progress for this run.",
            requestId,
            { activeTurn: activeExecution.turn },
          );
          return;
        }

        const started = await startTurnStepExecution(db, {
          runId,
          gameProjectId: runLocation.gameProjectId,
          moduleBindings: manifest.moduleBindings,
          turn,
          playerInput,
          playerId,
        });
        const pipelineState = await readPipelineState(runLocation.gameProjectId, runId, turn);
        res.json({
          runId,
          turn,
          execution: started,
          pipelineEvents: pipelineState.events,
        });
      } finally {
        await db.close();
      }
    } catch (error) {
      sendError(
        res,
        500,
        "STEP_START_FAILED",
        "Could not start stepped turn execution.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.post("/turn/step/next", async (req, res) => {
    const requestId = randomUUID();
    const { runId, turn } = req.body as {
      runId: string;
      turn: number;
    };
    if (!runId || !Number.isInteger(turn)) {
      sendError(
        res,
        400,
        "BAD_STEP_NEXT_REQUEST",
        "Missing or invalid step next payload fields.",
        requestId,
        "Expected runId and integer turn.",
      );
      return;
    }
    try {
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }
      const manifest = loadGameProjectManifest(runLocation.gameProjectId);
      const db = await openRunStore(runLocation.gameProjectId, runId);
      try {
        const execution = await getTurnExecutionState(db, runId, turn);
        if (!execution) {
          sendError(
            res,
            404,
            "STEP_EXECUTION_NOT_FOUND",
            "No active step execution found for this turn.",
            requestId,
            { runId, turn },
          );
          return;
        }
        const advanced = await advanceTurnStepExecution(db, {
          runId,
          gameProjectId: runLocation.gameProjectId,
          moduleBindings: manifest.moduleBindings,
          turn,
          playerInput: execution.playerInput,
          playerId: execution.playerId,
        });
        res.json({
          runId,
          turn,
          execution: advanced.state,
          pipelineEvents: advanced.pipelineEvents,
          result: advanced.result,
        });
      } finally {
        await db.close();
      }
    } catch (error) {
      sendError(
        res,
        500,
        "STEP_NEXT_FAILED",
        "Could not advance stepped turn execution.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.get("/run/:runId/turn/:turn/pipeline", async (req, res) => {
    const requestId = randomUUID();
    const runId = req.params.runId;
    const turn = Number(req.params.turn);
    if (!Number.isInteger(turn) || turn < 1) {
      sendError(res, 400, "INVALID_TURN_INDEX", "Turn must be >= 1.", requestId);
      return;
    }
    try {
      const runLocation = await resolveRunLocation(runId);
      if (!runLocation) {
        sendError(res, 404, "RUN_NOT_FOUND", "Run not found.", requestId, { runId });
        return;
      }
      const pipelineState = await readPipelineState(runLocation.gameProjectId, runId, turn);
      res.json(pipelineState);
    } catch (error) {
      sendError(
        res,
        500,
        "PIPELINE_STATE_READ_FAILED",
        "Could not read pipeline state.",
        requestId,
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  app.listen(port, () => {
    // Intentional single-line startup log for easy tooling parsing.
    console.log(
      `Morpheus backend listening on http://localhost:${port} (router mode)`,
    );
  });
}

main().catch((error) => {
  console.error("Backend startup failed:", error);
  process.exit(1);
});
