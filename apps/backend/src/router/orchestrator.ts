import { randomUUID } from "node:crypto";
import type { GameDb } from "../db.js";
import {
  ArbiterModuleRequestSchema,
  CommittedDiffSchema,
  IntentModuleRequestSchema,
  LoreRetrieverModuleRequestSchema,
  LoremasterPostModuleRequestSchema,
  LoremasterPostOutputSchema,
  LoremasterPreModuleRequestSchema,
  ModuleRunContextSchema,
  PipelineStepEventSchema,
  TurnExecutionStateSchema,
  ProserModuleRequestSchema,
  ProposedDiffSchema,
  SimulatorModuleRequestSchema,
  type ActionCandidates,
  type CommittedDiff,
  type PipelineStepEvent,
  type LoremasterOutput,
  type ProposedDiff,
  type TurnExecutionState,
} from "@morpheus/shared";
import {
  invokeArbiter,
  invokeIntent,
  invokeLorePost,
  invokeLorePre,
  invokeLoreRetrieve,
  invokeProser,
  invokeSimulator,
} from "./client.js";
import { resolveModuleUrls } from "./registry.js";
import {
  appendPipelineEvent,
  createTurnExecution,
  getTurnExecutionState,
  listPipelineEvents,
  readTurnExecutionCheckpoint,
  updateTurnExecutionProgress,
} from "../sessionStore.js";

export interface TurnInput {
  runId: string;
  gameProjectId: string;
  moduleBindings: Record<string, string>;
  turn: number;
  playerInput: string;
  playerId: string;
}

interface PipelineCheckpoint {
  intent?: ActionCandidates;
  loreRetrieval?: unknown;
  loremasterPre?: LoremasterOutput;
  proposal?: ProposedDiff;
  lorePost?: ReturnType<typeof buildFallbackLorePost>;
  committed?: CommittedDiff;
  arbiterDecision?: unknown;
  narrationText?: string;
  warnings: string[];
  llmConversations: Record<string, unknown>;
  refusalReason?: string;
}

type StepStage =
  | "intent_extractor"
  | "loremaster_retrieve"
  | "loremaster_pre"
  | "default_simulator"
  | "loremaster_post"
  | "arbiter"
  | "proser"
  | "world_state_update";

const STAGE_ORDER: StepStage[] = [
  "intent_extractor",
  "loremaster_retrieve",
  "loremaster_pre",
  "default_simulator",
  "loremaster_post",
  "arbiter",
  "proser",
  "world_state_update",
];

function deriveRefusalReasonFromIntent(intent: ActionCandidates): string | null {
  const rejectedCandidate = intent.candidates.find((candidate) => {
    const tags = candidate.consequenceTags ?? [];
    return tags.includes("no_target_in_scope");
  });
  if (!rejectedCandidate) return null;
  if ((rejectedCandidate.consequenceTags ?? []).includes("no_target_in_scope")) {
    if (rejectedCandidate.intent === "attack") return "Refused: no valid attack target is currently in scope.";
    return `Refused: no valid target is in scope for ${rejectedCandidate.intent.replace(/_/g, " ")}.`;
  }
  return "Refused: action is ambiguous and cannot be safely resolved.";
}

function deriveRefusalReasonFromLoremaster(
  intent: ActionCandidates,
  loremaster: LoremasterOutput,
): string | null {
  const assessment = loremaster.assessments.find((item) =>
    (item.consequenceTags ?? []).includes("no_target_in_scope"),
  );
  if (!assessment) return null;
  if (assessment.rationale?.trim()) {
    return `Refused: ${assessment.rationale.trim()}`;
  }
  const candidate = intent.candidates[assessment.candidateIndex];
  if (!candidate) return "Refused: action cannot be resolved safely.";
  return deriveRefusalReasonFromIntent({ candidates: [candidate], rawInput: intent.rawInput });
}

function arbiterCommit(turn: number, proposal: ProposedDiff): CommittedDiff {
  return CommittedDiffSchema.parse({
    turn,
    operations: proposal.operations,
    summary: "Action resolved with router-managed module pipeline.",
  });
}

function buildRefusalProposal(input: TurnInput, refusalReason: string): ProposedDiff {
  return ProposedDiffSchema.parse({
    moduleName: "refusal_handler",
    operations: [
      {
        op: "observation",
        scope: "view:player",
        payload: {
          observerId: input.playerId,
          text: refusalReason,
          turn: input.turn,
        },
        reason: "action refused by deterministic invalid-action policy",
      },
    ],
  });
}

function buildFallbackLorePost() {
  return LoremasterPostOutputSchema.parse({
    status: "consistent",
    rationale: "Post-diff lore check skipped due to refusal path.",
    mustInclude: [],
    mustAvoid: [],
  });
}

function nowIso(): string {
  return new Date().toISOString();
}

function emptyCheckpoint(): PipelineCheckpoint {
  return {
    warnings: [],
    llmConversations: {},
  };
}

function parseCheckpoint(value: unknown): PipelineCheckpoint {
  if (!value || typeof value !== "object") return emptyCheckpoint();
  const parsed = value as Partial<PipelineCheckpoint>;
  return {
    intent: parsed.intent,
    loreRetrieval: parsed.loreRetrieval,
    loremasterPre: parsed.loremasterPre,
    proposal: parsed.proposal,
    lorePost: parsed.lorePost,
    committed: parsed.committed,
    arbiterDecision: parsed.arbiterDecision,
    narrationText: parsed.narrationText,
    warnings: Array.isArray(parsed.warnings)
      ? parsed.warnings.filter((item): item is string => typeof item === "string")
      : [],
    llmConversations:
      parsed.llmConversations && typeof parsed.llmConversations === "object"
        ? parsed.llmConversations
        : {},
    refusalReason:
      typeof parsed.refusalReason === "string" ? parsed.refusalReason : undefined,
  };
}

function stageEndpoint(stage: StepStage): string {
  if (stage === "intent_extractor") return "/invoke";
  if (stage === "loremaster_retrieve") return "/retrieve";
  if (stage === "loremaster_pre") return "/pre";
  if (stage === "default_simulator") return "/invoke";
  if (stage === "loremaster_post") return "/post";
  if (stage === "arbiter") return "/invoke";
  if (stage === "proser") return "/invoke";
  return "persist";
}

function shouldSkipStage(stage: StepStage, checkpoint: PipelineCheckpoint): boolean {
  if (!checkpoint.refusalReason) return false;
  return stage === "default_simulator" || stage === "loremaster_post" || stage === "arbiter" || stage === "proser";
}

function resolveStageFromCursor(cursor: number): StepStage | null {
  if (cursor < 0 || cursor >= STAGE_ORDER.length) return null;
  return STAGE_ORDER[cursor];
}

function buildTurnTracePayload(checkpoint: PipelineCheckpoint, pipelineEvents: PipelineStepEvent[]) {
  return {
    intent: checkpoint.intent ?? null,
    loremaster: {
      retrieval: checkpoint.loreRetrieval ?? null,
      pre: checkpoint.loremasterPre ?? null,
      post: checkpoint.lorePost ?? buildFallbackLorePost(),
    },
    proposal: checkpoint.proposal ?? null,
    arbiter: checkpoint.arbiterDecision ?? null,
    committed: checkpoint.committed ?? null,
    refusal: checkpoint.refusalReason ? { reason: checkpoint.refusalReason } : null,
    warnings: checkpoint.warnings,
    narrationText: checkpoint.narrationText ?? "",
    pipelineEvents,
    llmConversations: checkpoint.llmConversations,
  };
}

async function appendStepEvent(
  db: GameDb,
  input: TurnInput,
  event: Omit<PipelineStepEvent, "stepNumber">,
): Promise<PipelineStepEvent> {
  const existing = await listPipelineEvents(db, input.runId, input.turn);
  const nextEvent = PipelineStepEventSchema.parse({
    ...event,
    stepNumber: existing.length + 1,
  });
  await appendPipelineEvent(db, input.runId, input.turn, nextEvent);
  return nextEvent;
}

async function runStage(
  db: GameDb,
  input: TurnInput,
  stage: StepStage,
  checkpoint: PipelineCheckpoint,
): Promise<PipelineCheckpoint> {
  const context = ModuleRunContextSchema.parse({
    requestId: randomUUID(),
    runId: input.runId,
    gameProjectId: input.gameProjectId,
    turn: input.turn,
    playerId: input.playerId,
    playerInput: input.playerInput,
  });
  const moduleUrls = resolveModuleUrls(input.moduleBindings);

  if (shouldSkipStage(stage, checkpoint)) {
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "skipped",
      request: { reason: "invalid action refusal path active", refusalReason: checkpoint.refusalReason ?? null },
      response: null,
      warnings: [],
      startedAt: nowIso(),
      finishedAt: nowIso(),
    });
    return checkpoint;
  }

  if (stage === "intent_extractor") {
    const startedAt = nowIso();
    const request = IntentModuleRequestSchema.parse({ context });
    const result = await invokeIntent(moduleUrls.intent, request);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      intent: result.output,
      refusalReason: deriveRefusalReasonFromIntent(result.output) ?? checkpoint.refusalReason,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        intent_extractor: result.debug.llmConversation,
      },
    };
  }

  if (stage === "loremaster_retrieve") {
    const startedAt = nowIso();
    const request = LoreRetrieverModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
    });
    const result = await invokeLoreRetrieve(moduleUrls.loremaster, request);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      loreRetrieval: result.output,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
    };
  }

  if (stage === "loremaster_pre") {
    const startedAt = nowIso();
    const request = LoremasterPreModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      lore: checkpoint.loreRetrieval,
    });
    const result = await invokeLorePre(moduleUrls.loremaster, request);
    const refusalReason = deriveRefusalReasonFromLoremaster(
      checkpoint.intent as ActionCandidates,
      result.output,
    );
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      loremasterPre: result.output,
      refusalReason: refusalReason ?? undefined,
      warnings: [
        ...checkpoint.warnings,
        ...result.meta.warnings,
        ...(refusalReason ? [`turn_refused: ${refusalReason}`] : []),
      ],
      llmConversations: {
        ...checkpoint.llmConversations,
        loremaster_pre: result.debug.llmConversation,
      },
    };
  }

  if (stage === "default_simulator") {
    const startedAt = nowIso();
    const request = SimulatorModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      lore: checkpoint.loreRetrieval,
      loremasterPre: checkpoint.loremasterPre,
    });
    const result = await invokeSimulator(moduleUrls.simulator, request);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      proposal: result.output,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        default_simulator: result.debug.llmConversation,
      },
    };
  }

  if (stage === "loremaster_post") {
    const startedAt = nowIso();
    const request = LoremasterPostModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      lore: checkpoint.loreRetrieval,
      proposal: checkpoint.proposal,
    });
    const result = await invokeLorePost(moduleUrls.loremaster, request);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      lorePost: result.output,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        loremaster_post: result.debug.llmConversation,
      },
    };
  }

  if (stage === "arbiter") {
    const startedAt = nowIso();
    const request = ArbiterModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      lore: checkpoint.loreRetrieval,
      loremasterPre: checkpoint.loremasterPre,
      proposal: checkpoint.proposal,
      lorePost: checkpoint.lorePost ?? buildFallbackLorePost(),
    });
    const result = await invokeArbiter(moduleUrls.arbiter, request);
    const selectedProposal = result.output.selectedProposal;
    const committed = arbiterCommit(input.turn, selectedProposal);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      proposal: selectedProposal,
      committed,
      arbiterDecision: result.output,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        arbiter: result.debug.llmConversation,
      },
    };
  }

  if (stage === "proser") {
    const startedAt = nowIso();
    const committedForProser = checkpoint.committed ?? arbiterCommit(input.turn, checkpoint.proposal as ProposedDiff);
    const request = ProserModuleRequestSchema.parse({
      context,
      committed: committedForProser,
      lore: checkpoint.loreRetrieval,
      lorePost: checkpoint.lorePost ?? buildFallbackLorePost(),
    });
    const result = await invokeProser(moduleUrls.proser, request);
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request,
      response: result,
      warnings: result.meta.warnings,
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      committed: committedForProser,
      narrationText: result.output.narrationText,
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        proser: result.debug.llmConversation,
      },
    };
  }

  const startedAt = nowIso();
  const refusalReason = checkpoint.refusalReason;
  const proposal = refusalReason
    ? buildRefusalProposal(input, refusalReason)
    : (checkpoint.proposal as ProposedDiff);
  const lorePost = refusalReason ? buildFallbackLorePost() : checkpoint.lorePost;
  const committed = checkpoint.committed ?? arbiterCommit(input.turn, proposal);
  const narrationText = refusalReason ?? checkpoint.narrationText ?? "";
  const finishedAt = nowIso();
  await appendStepEvent(db, input, {
    stage,
    endpoint: stageEndpoint(stage),
    status: "ok",
    request: { action: "persist_world_state", refusalReason: refusalReason ?? null },
    response: {
      committed,
      narrationText,
      persisted: true,
    },
    warnings: [],
    startedAt,
    finishedAt,
  });
  const persistedCheckpoint = {
    ...checkpoint,
    proposal,
    lorePost,
    committed,
    narrationText,
  };
  await persistTurnFinalState(db, input, persistedCheckpoint);
  return {
    ...persistedCheckpoint,
  };
}

async function persistTurnFinalState(db: GameDb, input: TurnInput, checkpoint: PipelineCheckpoint): Promise<void> {
  const pipelineEvents = await listPipelineEvents(db, input.runId, input.turn);
  const trace = buildTurnTracePayload(checkpoint, pipelineEvents);
  await db.run(
    `INSERT INTO events (turn, event_type, payload) VALUES (?, ?, ?)`,
    input.turn,
    "module_trace",
    JSON.stringify(trace),
  );
  await db.run(
    `INSERT INTO events (turn, event_type, payload) VALUES (?, ?, ?)`,
    input.turn,
    "committed_diff",
    JSON.stringify(checkpoint.committed),
  );
  await db.run(
    `INSERT INTO snapshots (turn, world_state, view_state) VALUES (?, ?, ?)`,
    input.turn,
    JSON.stringify({ lastSummary: checkpoint.committed?.summary ?? "" }),
    JSON.stringify({ lastObservation: checkpoint.committed?.operations ?? [] }),
  );
}

async function ensurePlayerInputRecorded(db: GameDb, input: TurnInput): Promise<void> {
  const existing = await db.get<{ count: number }>(
    `SELECT COUNT(*) as count FROM events WHERE turn = ? AND event_type = ?`,
    input.turn,
    "player_input",
  );
  if ((existing?.count ?? 0) > 0) return;
  await db.run(
    `INSERT INTO events (turn, event_type, payload) VALUES (?, ?, ?)`,
    input.turn,
    "player_input",
    JSON.stringify({ id: randomUUID(), text: input.playerInput }),
  );
}

export async function startTurnStepExecution(db: GameDb, input: TurnInput): Promise<TurnExecutionState> {
  const existing = await getTurnExecutionState(db, input.runId, input.turn);
  if (existing && !existing.completed) return existing;
  if (existing && existing.completed) {
    throw new Error("Step execution already completed for this turn.");
  }

  await ensurePlayerInputRecorded(db, input);
  await appendStepEvent(db, input, {
    stage: "frontend_input",
    endpoint: "frontend",
    status: "ok",
    request: {
      playerInput: input.playerInput,
      playerId: input.playerId,
      turn: input.turn,
    },
    response: { accepted: true },
    warnings: [],
    startedAt: nowIso(),
    finishedAt: nowIso(),
  });

  const state = await createTurnExecution(db, {
    runId: input.runId,
    turn: input.turn,
    mode: "step",
    playerInput: input.playerInput,
    playerId: input.playerId,
    requestId: randomUUID(),
    gameProjectId: input.gameProjectId,
    checkpoint: emptyCheckpoint(),
  });
  return TurnExecutionStateSchema.parse(state);
}

export async function advanceTurnStepExecution(
  db: GameDb,
  input: TurnInput,
): Promise<{ state: TurnExecutionState; pipelineEvents: PipelineStepEvent[]; result: ReturnType<typeof buildTurnTracePayload> | null }> {
  const execution = await getTurnExecutionState(db, input.runId, input.turn);
  if (!execution) {
    throw new Error("No active stepped execution found. Start step mode first.");
  }
  if (execution.completed) {
    const pipelineEvents = await listPipelineEvents(db, input.runId, input.turn);
    const checkpoint = parseCheckpoint(await readTurnExecutionCheckpoint(db, input.runId, input.turn));
    return {
      state: execution,
      pipelineEvents,
      result: checkpoint.committed ? buildTurnTracePayload(checkpoint, pipelineEvents) : null,
    };
  }

  const stage = resolveStageFromCursor(execution.cursor);
  if (!stage) {
    throw new Error("Invalid step cursor.");
  }

  const checkpoint = parseCheckpoint(await readTurnExecutionCheckpoint(db, input.runId, input.turn));
  const nextCheckpoint = await runStage(db, input, stage, checkpoint);
  const completed = stage === "world_state_update";

  const nextState = await updateTurnExecutionProgress(db, {
    runId: input.runId,
    turn: input.turn,
    cursor: execution.cursor + 1,
    checkpoint: nextCheckpoint,
    completed,
    narrationText: nextCheckpoint.narrationText,
    warnings: nextCheckpoint.warnings,
  });
  const pipelineEvents = await listPipelineEvents(db, input.runId, input.turn);
  return {
    state: TurnExecutionStateSchema.parse(nextState),
    pipelineEvents,
    result: completed ? buildTurnTracePayload(nextCheckpoint, pipelineEvents) : null,
  };
}

export async function processTurnViaRouter(db: GameDb, input: TurnInput) {
  await ensurePlayerInputRecorded(db, input);
  await appendStepEvent(db, input, {
    stage: "frontend_input",
    endpoint: "frontend",
    status: "ok",
    request: {
      playerInput: input.playerInput,
      playerId: input.playerId,
      turn: input.turn,
    },
    response: { accepted: true },
    warnings: [],
    startedAt: nowIso(),
    finishedAt: nowIso(),
  });

  await createTurnExecution(db, {
    runId: input.runId,
    turn: input.turn,
    mode: "normal",
    playerInput: input.playerInput,
    playerId: input.playerId,
    requestId: randomUUID(),
    gameProjectId: input.gameProjectId,
    checkpoint: emptyCheckpoint(),
  });

  let checkpoint = emptyCheckpoint();
  for (const stage of STAGE_ORDER) {
    checkpoint = await runStage(db, input, stage, checkpoint);
  }
  await updateTurnExecutionProgress(db, {
    runId: input.runId,
    turn: input.turn,
    cursor: STAGE_ORDER.length,
    checkpoint,
    completed: true,
    narrationText: checkpoint.narrationText,
    warnings: checkpoint.warnings,
  });

  return {
    intent: checkpoint.intent,
    loremaster: {
      retrieval: checkpoint.loreRetrieval,
      pre: checkpoint.loremasterPre,
      post: checkpoint.lorePost ?? buildFallbackLorePost(),
    },
    proposal: checkpoint.proposal,
    committed: checkpoint.committed,
    warnings: checkpoint.warnings,
    narrationText: checkpoint.narrationText ?? "",
  };
}
