import test from "node:test";
import assert from "node:assert/strict";
import {
  ArbiterModuleRequestSchema,
  ArbiterModuleResponseSchema,
  IntentModuleRequestSchema,
  IntentModuleResponseSchema,
  LoreRetrieverModuleRequestSchema,
  LoreRetrieverModuleResponseSchema,
  LoremasterPostModuleRequestSchema,
  LoremasterPostModuleResponseSchema,
  LoremasterPreModuleRequestSchema,
  LoremasterPreModuleResponseSchema,
  PipelineStepEventSchema,
  ProserModuleRequestSchema,
  ProserModuleResponseSchema,
  SimulatorModuleRequestSchema,
  SimulatorModuleResponseSchema,
  TurnExecutionStateSchema,
} from "@morpheus/shared";

const context = {
  requestId: "req-1",
  runId: "run-1",
  gameProjectId: "sandcrawler",
  turn: 1,
  playerId: "entity.player.captain",
  playerInput: "Look around.",
};

const intentOutput = {
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
};

const loreOutput = {
  query: "Look around.",
  evidence: [{ source: "lore/world.md", excerpt: "Crawler over dunes", score: 4 }],
  summary: "Retrieved lore excerpts.",
};

const proposalOutput = {
  moduleName: "default_simulator",
  operations: [
    {
      op: "observation",
      scope: "view:player",
      payload: { observerId: "entity.player.captain", text: "You scan the horizon.", turn: 1 },
      reason: "Grounded observation.",
    },
  ],
};

test("module request/response contracts parse valid payloads", () => {
  assert.doesNotThrow(() => IntentModuleRequestSchema.parse({ context }));
  assert.doesNotThrow(() =>
    IntentModuleResponseSchema.parse({
      meta: { moduleName: "intent_extractor", warnings: [] },
      output: intentOutput,
      debug: {},
    }),
  );

  assert.doesNotThrow(() => LoreRetrieverModuleRequestSchema.parse({ context, intent: intentOutput }));
  assert.doesNotThrow(() =>
    LoreRetrieverModuleResponseSchema.parse({
      meta: { moduleName: "loremaster.retrieve", warnings: [] },
      output: loreOutput,
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    LoremasterPreModuleRequestSchema.parse({
      context,
      intent: intentOutput,
      lore: loreOutput,
    }),
  );
  assert.doesNotThrow(() =>
    LoremasterPreModuleResponseSchema.parse({
      meta: { moduleName: "loremaster_pre", warnings: [] },
      output: {
        assessments: [
          {
            candidateIndex: 0,
            status: "allowed",
            consequenceTags: [],
            rationale: "Valid.",
          },
        ],
        summary: "Allowed.",
      },
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    SimulatorModuleRequestSchema.parse({
      context,
      intent: intentOutput,
      loremasterPre: {
        assessments: [
          { candidateIndex: 0, status: "allowed", consequenceTags: [], rationale: "Valid." },
        ],
        summary: "Allowed.",
      },
      lore: loreOutput,
    }),
  );
  assert.doesNotThrow(() =>
    SimulatorModuleResponseSchema.parse({
      meta: { moduleName: "default_simulator", warnings: [] },
      output: proposalOutput,
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    LoremasterPostModuleRequestSchema.parse({
      context,
      intent: intentOutput,
      lore: loreOutput,
      proposal: proposalOutput,
    }),
  );
  assert.doesNotThrow(() =>
    LoremasterPostModuleResponseSchema.parse({
      meta: { moduleName: "loremaster_post", warnings: [] },
      output: {
        status: "consistent",
        rationale: "Consistent.",
        mustInclude: ["desert"],
        mustAvoid: ["ocean"],
      },
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    ArbiterModuleRequestSchema.parse({
      context,
      intent: intentOutput,
      lore: loreOutput,
      loremasterPre: {
        assessments: [{ candidateIndex: 0, status: "allowed", consequenceTags: [], rationale: "Valid." }],
        summary: "Allowed.",
      },
      proposal: proposalOutput,
      lorePost: {
        status: "consistent",
        rationale: "Consistent.",
        mustInclude: ["desert"],
        mustAvoid: ["ocean"],
      },
    }),
  );
  assert.doesNotThrow(() =>
    ArbiterModuleResponseSchema.parse({
      meta: { moduleName: "arbiter", warnings: [] },
      output: {
        decision: "accept",
        selectedProposal: proposalOutput,
        rationale: "Accepted.",
        rerunHints: [],
        selectionMetadata: {},
      },
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    ProserModuleRequestSchema.parse({
      context,
      committed: {
        turn: 1,
        operations: proposalOutput.operations,
        summary: "Committed.",
      },
      lore: loreOutput,
      lorePost: {
        status: "consistent",
        rationale: "Consistent.",
        mustInclude: ["desert"],
        mustAvoid: ["ocean"],
      },
    }),
  );
  assert.doesNotThrow(() =>
    ProserModuleResponseSchema.parse({
      meta: { moduleName: "proser", warnings: [] },
      output: { narrationText: "You scan the dunes." },
      debug: {},
    }),
  );

  assert.doesNotThrow(() =>
    PipelineStepEventSchema.parse({
      stepNumber: 1,
      stage: "intent_extractor",
      endpoint: "/invoke",
      status: "ok",
      request: { context },
      response: { ok: true },
      warnings: [],
      startedAt: new Date().toISOString(),
      finishedAt: new Date().toISOString(),
    }),
  );

  assert.doesNotThrow(() =>
    TurnExecutionStateSchema.parse({
      runId: "run-1",
      turn: 1,
      mode: "step",
      cursor: 2,
      completed: false,
      createdAt: new Date().toISOString(),
      updatedAt: new Date().toISOString(),
      playerInput: "Look around.",
      playerId: "entity.player.captain",
      requestId: "req-step",
      gameProjectId: "sandcrawler",
      result: { warnings: [] },
    }),
  );
});
