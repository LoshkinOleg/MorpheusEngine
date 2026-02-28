import test from "node:test";
import assert from "node:assert/strict";
import sqlite3 from "sqlite3";
import { open } from "sqlite";
import { processTurnViaRouter } from "../apps/backend/dist/router/orchestrator.js";

async function createInMemoryRunDb() {
  const db = await open({
    filename: ":memory:",
    driver: sqlite3.Database,
  });
  await db.exec(`
    CREATE TABLE events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      turn INTEGER NOT NULL,
      event_type TEXT NOT NULL,
      payload TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE snapshots (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      turn INTEGER NOT NULL,
      world_state TEXT NOT NULL,
      view_state TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
    CREATE TABLE turn_execution (
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
    CREATE TABLE pipeline_events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      run_id TEXT NOT NULL,
      turn INTEGER NOT NULL,
      step_number INTEGER NOT NULL,
      payload TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );
  `);
  await db.run(
    `INSERT INTO snapshots (turn, world_state, view_state) VALUES (?, ?, ?)`,
    0,
    JSON.stringify({}),
    JSON.stringify({}),
  );
  return db;
}

test("router pipeline processes full turn through module calls", async () => {
  const db = await createInMemoryRunDb();
  const originalFetch = globalThis.fetch;
  process.env.MODULE_INTENT_URL = "http://intent";
  process.env.MODULE_LOREMASTER_URL = "http://loremaster";
  process.env.MODULE_DEFAULT_SIMULATOR_URL = "http://simulator";
  process.env.MODULE_ARBITER_URL = "http://arbiter";
  process.env.MODULE_PROSER_URL = "http://proser";

  globalThis.fetch = async (url) => {
    const target = String(url);
    if (target.startsWith("http://intent")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "intent_extractor", warnings: [] },
          output: {
            rawInput: "Look around.",
            candidates: [
              {
                actorId: "entity.player.captain",
                intent: "inspect_environment",
                confidence: 0.9,
                params: {},
                consequenceTags: [],
              },
            ],
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://loremaster/retrieve")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "loremaster.retrieve", warnings: [] },
          output: {
            query: "Look around.",
            evidence: [{ source: "lore/world.md", excerpt: "Desert crawler world", score: 4 }],
            summary: "Retrieved lore.",
          },
          debug: {},
        }),
      );
    }
    if (target.startsWith("http://loremaster/pre")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "loremaster_pre", warnings: [] },
          output: {
            assessments: [
              {
                candidateIndex: 0,
                status: "allowed",
                consequenceTags: [],
                rationale: "Valid action.",
              },
            ],
            summary: "Allowed.",
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://simulator")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "default_simulator", warnings: [] },
          output: {
            moduleName: "default_simulator",
            operations: [
              {
                op: "observation",
                scope: "view:player",
                payload: { observerId: "entity.player.captain", text: "You scan the desert.", turn: 1 },
                reason: "Scoped narrative observation.",
              },
            ],
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://loremaster/post")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "loremaster_post", warnings: [] },
          output: {
            status: "consistent",
            rationale: "Consistent with lore.",
            mustInclude: ["desert", "crawler"],
            mustAvoid: ["ocean"],
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://proser")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "proser", warnings: [] },
          output: { narrationText: "Dust sweeps across the crawler deck as you survey the dunes." },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://arbiter")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "arbiter", warnings: [] },
          output: {
            decision: "accept",
            selectedProposal: {
              moduleName: "default_simulator",
              operations: [
                {
                  op: "observation",
                  scope: "view:player",
                  payload: { observerId: "entity.player.captain", text: "You scan the desert.", turn: 1 },
                  reason: "Scoped narrative observation.",
                },
              ],
            },
            rationale: "Accepted validated proposal.",
            rerunHints: [],
            selectionMetadata: {},
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    return new Response("{}", { status: 404 });
  };

  try {
    const result = await processTurnViaRouter(db, {
      runId: "run-1",
      gameProjectId: "sandcrawler",
      moduleBindings: {
        intent_extractor: "llm.default",
        loremaster: "llm.default",
        default_simulator: "llm.default",
        arbiter: "llm.default",
        proser: "llm.default",
      },
      turn: 1,
      playerInput: "Look around.",
      playerId: "entity.player.captain",
    });

    assert.equal(typeof result.narrationText, "string");
    assert.equal(result.narrationText.includes("crawler"), true);
    const events = await db.all(`SELECT event_type FROM events ORDER BY id ASC`);
    assert.equal(events.length, 3);
    assert.deepEqual(events.map((row) => row.event_type), ["player_input", "module_trace", "committed_diff"]);
    const pipelineEvents = await db.all(`SELECT step_number FROM pipeline_events ORDER BY step_number ASC`);
    assert.equal(pipelineEvents.length, 9);
    const traceRow = await db.get(`SELECT payload FROM events WHERE event_type = 'module_trace' ORDER BY id DESC LIMIT 1`);
    const trace = JSON.parse(traceRow.payload);
    const stages = trace.pipelineEvents.map((event) => event.stage);
    assert.equal(stages.indexOf("arbiter") < stages.indexOf("proser"), true);
  } finally {
    globalThis.fetch = originalFetch;
    await db.close();
  }
});

test("router refuses invalid action with in-world narration and no simulation path", async () => {
  const db = await createInMemoryRunDb();
  const originalFetch = globalThis.fetch;
  process.env.MODULE_INTENT_URL = "http://intent";
  process.env.MODULE_LOREMASTER_URL = "http://loremaster";
  process.env.MODULE_DEFAULT_SIMULATOR_URL = "http://simulator";
  process.env.MODULE_ARBITER_URL = "http://arbiter";
  process.env.MODULE_PROSER_URL = "http://proser";

  let simulatorCalled = false;
  let arbiterCalled = false;
  let proserCalled = false;
  let lorePostCalled = false;

  globalThis.fetch = async (url) => {
    const target = String(url);
    if (target.startsWith("http://intent")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "intent_extractor", warnings: [] },
          output: {
            rawInput: "Attack.",
            candidates: [
              {
                actorId: "entity.player.captain",
                intent: "attack",
                confidence: 0.9,
                params: {},
                consequenceTags: ["no_target_in_scope"],
              },
            ],
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://loremaster/retrieve")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "loremaster.retrieve", warnings: [] },
          output: {
            query: "Attack.",
            evidence: [{ source: "lore/world.md", excerpt: "No enemies nearby", score: 3 }],
            summary: "Retrieved lore.",
          },
          debug: {},
        }),
      );
    }
    if (target.startsWith("http://loremaster/pre")) {
      return new Response(
        JSON.stringify({
          meta: { moduleName: "loremaster_pre", warnings: [] },
          output: {
            assessments: [
              {
                candidateIndex: 0,
                status: "allowed_with_consequences",
                consequenceTags: ["no_target_in_scope"],
                rationale: "No valid target is in scope.",
              },
            ],
            summary: "Refuse action.",
          },
          debug: { llmConversation: { attempts: [], usedFallback: false } },
        }),
      );
    }
    if (target.startsWith("http://simulator")) {
      simulatorCalled = true;
      return new Response("{}", { status: 500 });
    }
    if (target.startsWith("http://loremaster/post")) {
      lorePostCalled = true;
      return new Response("{}", { status: 500 });
    }
    if (target.startsWith("http://arbiter")) {
      arbiterCalled = true;
      return new Response("{}", { status: 500 });
    }
    if (target.startsWith("http://proser")) {
      proserCalled = true;
      return new Response("{}", { status: 500 });
    }
    return new Response("{}", { status: 404 });
  };

  try {
    const result = await processTurnViaRouter(db, {
      runId: "run-2",
      gameProjectId: "sandcrawler",
      moduleBindings: {
        intent_extractor: "llm.default",
        loremaster: "llm.default",
        default_simulator: "llm.default",
        arbiter: "llm.default",
        proser: "llm.default",
      },
      turn: 1,
      playerInput: "Attack.",
      playerId: "entity.player.captain",
    });

    assert.equal(simulatorCalled, false);
    assert.equal(lorePostCalled, false);
    assert.equal(arbiterCalled, false);
    assert.equal(proserCalled, false);
    assert.equal(result.narrationText.startsWith("Refused:"), true);
    assert.equal(Array.isArray(result.committed.operations), true);
    assert.equal(result.committed.operations.length, 1);
    assert.equal(result.committed.operations[0].scope, "view:player");

    const traceRow = await db.get(`SELECT payload FROM events WHERE event_type = 'module_trace' ORDER BY id DESC LIMIT 1`);
    const trace = JSON.parse(traceRow.payload);
    assert.equal(typeof trace.refusal?.reason, "string");
    assert.equal(trace.refusal.reason.startsWith("Refused:"), true);
    const stages = trace.pipelineEvents.map((event) => event.stage);
    assert.equal(stages.includes("default_simulator"), true);
    assert.equal(stages.includes("world_state_update"), true);
    const skippedStages = trace.pipelineEvents
      .filter((event) => event.status === "skipped")
      .map((event) => event.stage);
    assert.deepEqual(skippedStages, ["default_simulator", "loremaster_post", "arbiter", "proser"]);
  } finally {
    globalThis.fetch = originalFetch;
    await db.close();
  }
});
