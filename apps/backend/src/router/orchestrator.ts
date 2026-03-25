import { randomUUID } from "node:crypto";
import type { GameDb } from "../db.js";
import {
  ArbiterModuleRequestSchema,
  CommittedDiffSchema,
  IntentModuleRequestSchema,
  IntentValidatorModuleRequestSchema,
  LoreAccumulationSchema,
  ModuleRunContextSchema,
  NarrativeCapsuleSchema,
  PipelineStepEventSchema,
  TurnExecutionStateSchema,
  ProserModuleRequestSchema,
  ProposedDiffSchema,
  SimulatorModuleRequestSchema,
  type ActionCandidates,
  type CommittedDiff,
  type IntentValidatorOutput,
  type LoreAccumulation,
  type NarrativeCapsule,
  type PipelineStepEvent,
  type ProposedDiff,
  type ProserContextTraceSummary,
  type TurnExecutionState,
} from "@morpheus/shared";
import {
  invokeArbiter,
  invokeIntent,
  invokeIntentValidator,
  invokeProser,
  invokeSimulator,
} from "./client.js";
import { resolveModuleUrls } from "./registry.js";
import {
  appendPipelineEvent,
  createTurnExecution,
  getTurnExecutionState,
  listPipelineEvents,
  readLatestSnapshot,
  readNarrativeCapsule,
  readTurnExecutionCheckpoint,
  updateTurnExecutionProgress,
  writeNarrativeCapsule,
} from "../sessionStore.js";
import { assembleProserContext } from "./proserContextAssembler.js";

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
  intentValidation?: IntentValidatorOutput;
  proposal?: ProposedDiff;
  committed?: CommittedDiff;
  arbiterDecision?: unknown;
  narrationText?: string;
  warnings: string[];
  llmConversations: Record<string, unknown>;
  refusalReason?: string;
  loreAccumulation?: LoreAccumulation;
  proserPreparedRequest?: ReturnType<typeof ProserModuleRequestSchema.parse>;
  proserContextTrace?: ProserContextTraceSummary;
}

type StepStage =
  | "intent_extractor"
  | "intent_validator"
  | "default_simulator"
  | "arbiter"
  | "context_assembler"
  | "proser"
  | "world_state_update";

const STAGE_ORDER: StepStage[] = [
  "intent_extractor",
  "intent_validator",
  "default_simulator",
  "arbiter",
  "context_assembler",
  "proser",
  "world_state_update",
];

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

function nowIso(): string {
  return new Date().toISOString();
}

function mergeLoreKeyLists(...lists: string[][]): string[] {
  const set = new Set<string>();
  for (const list of lists) {
    for (const key of list) {
      const trimmed = key.trim();
      if (trimmed.length > 0) set.add(trimmed);
    }
  }
  return [...set];
}

function parseLoreAccumulation(value: unknown): LoreAccumulation {
  const parsed = LoreAccumulationSchema.safeParse(value);
  if (parsed.success) return parsed.data;
  return { loreKeys: [] };
}

function emptyCheckpoint(): PipelineCheckpoint {
  return {
    warnings: [],
    llmConversations: {},
    loreAccumulation: { loreKeys: [] },
  };
}

function parseCheckpoint(value: unknown): PipelineCheckpoint {
  if (!value || typeof value !== "object") return emptyCheckpoint();
  const parsed = value as Partial<PipelineCheckpoint>;
  const base = emptyCheckpoint();
  let proserPrepared: PipelineCheckpoint["proserPreparedRequest"];
  if (parsed.proserPreparedRequest !== undefined) {
    const pr = ProserModuleRequestSchema.safeParse(parsed.proserPreparedRequest);
    proserPrepared = pr.success ? pr.data : undefined;
  }
  return {
    intent: parsed.intent,
    intentValidation: parsed.intentValidation,
    proposal: parsed.proposal,
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
    loreAccumulation: parseLoreAccumulation(parsed.loreAccumulation ?? base.loreAccumulation),
    proserPreparedRequest: proserPrepared,
    proserContextTrace: parsed.proserContextTrace as ProserContextTraceSummary | undefined,
  };
}

function stageEndpoint(stage: StepStage): string {
  if (stage === "intent_extractor") return "/invoke";
  if (stage === "intent_validator") return "/invoke";
  if (stage === "default_simulator") return "/invoke";
  if (stage === "arbiter") return "/invoke";
  if (stage === "context_assembler") return "internal";
  if (stage === "proser") return "/invoke";
  return "persist";
}

function shouldSkipStage(stage: StepStage, checkpoint: PipelineCheckpoint): boolean {
  if (!checkpoint.refusalReason) return false;
  return (
    stage === "default_simulator" ||
    stage === "arbiter" ||
    stage === "context_assembler" ||
    stage === "proser"
  );
}

function resolveStageFromCursor(cursor: number): StepStage | null {
  if (cursor < 0 || cursor >= STAGE_ORDER.length) return null;
  return STAGE_ORDER[cursor];
}

function buildTurnTracePayload(checkpoint: PipelineCheckpoint, pipelineEvents: PipelineStepEvent[]) {
  return {
    intent: checkpoint.intent ?? null,
    intentValidator: checkpoint.intentValidation ?? null,
    proposal: checkpoint.proposal ?? null,
    arbiter: checkpoint.arbiterDecision ?? null,
    committed: checkpoint.committed ?? null,
    refusal: checkpoint.refusalReason ? { reason: checkpoint.refusalReason } : null,
    warnings: checkpoint.warnings,
    narrationText: checkpoint.narrationText ?? "",
    pipelineEvents,
    llmConversations: checkpoint.llmConversations,
    loreAccumulation: checkpoint.loreAccumulation ?? { loreKeys: [] },
    proserContext: checkpoint.proserContextTrace ?? null,
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
      warnings: [...checkpoint.warnings, ...result.meta.warnings],
      llmConversations: {
        ...checkpoint.llmConversations,
        intent_extractor: result.debug.llmConversation,
      },
    };
  }

  if (stage === "intent_validator") {
    const startedAt = nowIso();
    const request = IntentValidatorModuleRequestSchema.parse({
      context,
      intent: {
        candidates: (checkpoint.intent as ActionCandidates).candidates,
      },
    });
    const result = await invokeIntentValidator(moduleUrls.intentValidator, request);
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
    const refusalReason =
      result.output.decision === "refuse"
        ? result.output.refusalReason ?? `Refused: ${result.output.rationale}`
        : undefined;
    const mergedLoreKeys = mergeLoreKeyLists(
      checkpoint.loreAccumulation?.loreKeys ?? [],
      result.output.loreKeysUsed ?? [],
    );
    return {
      ...checkpoint,
      intentValidation: result.output,
      refusalReason,
      loreAccumulation: { loreKeys: mergedLoreKeys },
      warnings: [
        ...checkpoint.warnings,
        ...result.meta.warnings,
        ...(refusalReason ? [`turn_refused: ${refusalReason}`] : []),
      ],
      llmConversations: {
        ...checkpoint.llmConversations,
        intent_validator: result.debug.llmConversation,
      },
    };
  }

  if (stage === "default_simulator") {
    const startedAt = nowIso();
    const request = SimulatorModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      intentValidation: checkpoint.intentValidation,
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

  if (stage === "arbiter") {
    const startedAt = nowIso();
    const request = ArbiterModuleRequestSchema.parse({
      context,
      intent: checkpoint.intent,
      intentValidation: checkpoint.intentValidation,
      proposal: checkpoint.proposal,
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

  if (stage === "context_assembler") {
    const startedAt = nowIso();
    const committedForAssembler =
      checkpoint.committed ?? arbiterCommit(input.turn, checkpoint.proposal as ProposedDiff);
    const { request: proserRequest, traceSummary } = await assembleProserContext({
      db,
      context,
      loreAccumulation: checkpoint.loreAccumulation ?? { loreKeys: [] },
      intent: checkpoint.intent as ActionCandidates,
      intentValidation: checkpoint.intentValidation as IntentValidatorOutput,
      committed: committedForAssembler,
    });
    const finishedAt = nowIso();
    await appendStepEvent(db, input, {
      stage,
      endpoint: stageEndpoint(stage),
      status: "ok",
      request: {
        accumulatedLoreKeys: proserRequest.lore.decisionKeys.length,
        flavorKeysPlanned: proserRequest.lore.flavorKeys.length,
      },
      response: { traceSummary },
      warnings: [],
      startedAt,
      finishedAt,
    });
    return {
      ...checkpoint,
      committed: committedForAssembler,
      proserPreparedRequest: proserRequest,
      proserContextTrace: traceSummary,
    };
  }

  if (stage === "proser") {
    const startedAt = nowIso();
    const committedForProser = checkpoint.committed ?? arbiterCommit(input.turn, checkpoint.proposal as ProposedDiff);
    const request =
      checkpoint.proserPreparedRequest ??
      ProserModuleRequestSchema.parse({
        context,
        committed: committedForProser,
        validatedIntent: { candidates: (checkpoint.intent as ActionCandidates).candidates },
        lore: {
          decisionKeys: [],
          decisionExcerpts: [],
          flavorKeys: [],
          flavorExcerpts: [],
        },
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
    committed,
    narrationText,
  };
  await persistTurnFinalState(db, input, persistedCheckpoint);
  return {
    ...persistedCheckpoint,
  };
}

function mergeNarrativeCapsuleAfterProser(
  previous: NarrativeCapsule | null,
  narrationText: string,
  turn: number,
): NarrativeCapsule {
  const snippet = narrationText.trim().slice(0, 280);
  const line = `T${turn}: ${snippet}`;
  const prior = previous?.rollingSummary ?? "";
  const rolling = [prior, line].filter(Boolean).join("\n").slice(-2500);
  return NarrativeCapsuleSchema.parse({
    rollingSummary: rolling,
    tension: previous?.tension ?? "calm",
    lastTurnsDigest: snippet,
  });
}

async function persistTurnFinalState(db: GameDb, input: TurnInput, checkpoint: PipelineCheckpoint): Promise<void> {
  if (!checkpoint.refusalReason && typeof checkpoint.narrationText === "string" && checkpoint.narrationText.trim()) {
    const previousCapsule = await readNarrativeCapsule(db);
    const nextCapsule = mergeNarrativeCapsuleAfterProser(
      previousCapsule,
      checkpoint.narrationText,
      input.turn,
    );
    await writeNarrativeCapsule(db, nextCapsule);
  }

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

  const previousSnapshot = await readLatestSnapshot(db);
  const nextSnapshot = applyCommittedDiffToSnapshot(
    {
      worldState: previousSnapshot.worldState,
      viewState: previousSnapshot.viewState,
    },
    checkpoint.committed,
    input,
  );

  await db.run(
    `INSERT INTO snapshots (turn, world_state, view_state) VALUES (?, ?, ?)`,
    input.turn,
    JSON.stringify(nextSnapshot.worldState),
    JSON.stringify(nextSnapshot.viewState),
  );
}

type StoredEntity = Record<string, unknown> & { id?: string };
type StoredFact = {
  subjectId: string;
  key: string;
  value: unknown;
  confidence?: number;
  source?: string;
  stability?: string;
};

function toEntityMap(worldState: unknown): Record<string, StoredEntity> {
  if (!worldState || typeof worldState !== "object") return {};
  const maybeEntities = (worldState as { entities?: unknown }).entities;
  if (maybeEntities && typeof maybeEntities === "object" && !Array.isArray(maybeEntities)) {
    return { ...(maybeEntities as Record<string, StoredEntity>) };
  }
  if (Array.isArray(maybeEntities)) {
    const map: Record<string, StoredEntity> = {};
    for (const entity of maybeEntities) {
      if (!entity || typeof entity !== "object") continue;
      const id = (entity as { id?: unknown }).id;
      if (typeof id !== "string" || id.trim().length === 0) continue;
      map[id] = entity as StoredEntity;
    }
    return map;
  }
  return {};
}

function toFactsList(worldState: unknown): StoredFact[] {
  if (!worldState || typeof worldState !== "object") return [];
  const maybeFacts = (worldState as { facts?: unknown }).facts;
  if (!Array.isArray(maybeFacts)) return [];
  return maybeFacts.filter((fact): fact is StoredFact => {
    if (!fact || typeof fact !== "object") return false;
    const parsed = fact as { subjectId?: unknown; key?: unknown };
    return typeof parsed.subjectId === "string" && typeof parsed.key === "string";
  });
}

function toAnchors(worldState: unknown): string[] {
  if (!worldState || typeof worldState !== "object") return [];
  const maybeAnchors = (worldState as { anchors?: unknown }).anchors;
  if (!Array.isArray(maybeAnchors)) return [];
  return maybeAnchors.filter((anchor): anchor is string => typeof anchor === "string");
}

function toPlayerObservations(viewState: unknown): Array<Record<string, unknown>> {
  if (!viewState || typeof viewState !== "object") return [];
  const player = (viewState as { player?: unknown }).player;
  if (!player || typeof player !== "object") return [];
  const observations = (player as { observations?: unknown }).observations;
  if (!Array.isArray(observations)) return [];
  return observations.filter(
    (observation): observation is Record<string, unknown> =>
      !!observation && typeof observation === "object",
  );
}

function applyCommittedDiffToSnapshot(
  snapshot: { worldState: unknown; viewState: unknown },
  committed: CommittedDiff | undefined,
  input: TurnInput,
): { worldState: Record<string, unknown>; viewState: Record<string, unknown> } {
  const entities = toEntityMap(snapshot.worldState);
  const facts = [...toFactsList(snapshot.worldState)];
  const anchors = toAnchors(snapshot.worldState);
  const playerObservations = [...toPlayerObservations(snapshot.viewState)];

  const operations = committed?.operations ?? [];
  for (const operation of operations) {
    const payload =
      operation.payload && typeof operation.payload === "object"
        ? (operation.payload as Record<string, unknown>)
        : {};

    if (operation.op === "upsert_entity") {
      const entityCandidate =
        payload.entity && typeof payload.entity === "object"
          ? (payload.entity as Record<string, unknown>)
          : payload;
      const entityId = typeof entityCandidate.id === "string" ? entityCandidate.id : undefined;
      if (entityId && entityId.trim().length > 0) {
        entities[entityId] = { ...entities[entityId], ...entityCandidate };
      }
      const anchorId = payload.anchorId;
      if (typeof anchorId === "string" && anchorId.trim().length > 0 && !anchors.includes(anchorId)) {
        anchors.push(anchorId);
      }
      continue;
    }

    if (operation.op === "upsert_fact") {
      const subjectId = typeof payload.subjectId === "string" ? payload.subjectId : undefined;
      const key = typeof payload.key === "string" ? payload.key : undefined;
      if (!subjectId || !key) continue;
      const replacement: StoredFact = {
        subjectId,
        key,
        value: payload.value,
        ...(typeof payload.confidence === "number" ? { confidence: payload.confidence } : {}),
        ...(typeof payload.source === "string" ? { source: payload.source } : {}),
        ...(typeof payload.stability === "string" ? { stability: payload.stability } : {}),
      };
      const index = facts.findIndex((fact) => fact.subjectId === subjectId && fact.key === key);
      if (index >= 0) facts[index] = replacement;
      else facts.push(replacement);
      continue;
    }

    if (operation.op === "remove_fact") {
      const subjectId = typeof payload.subjectId === "string" ? payload.subjectId : undefined;
      const key = typeof payload.key === "string" ? payload.key : undefined;
      if (!subjectId || !key) continue;
      const index = facts.findIndex((fact) => fact.subjectId === subjectId && fact.key === key);
      if (index >= 0) facts.splice(index, 1);
      continue;
    }

    if (operation.op === "observation" || operation.op === "detection") {
      const observation = {
        op: operation.op,
        scope: operation.scope,
        payload,
        reason: operation.reason,
        turn: input.turn,
      };
      if (operation.scope === "view:player") {
        playerObservations.push(observation);
      }
    }
  }

  const worldState = {
    ...(snapshot.worldState && typeof snapshot.worldState === "object"
      ? (snapshot.worldState as Record<string, unknown>)
      : {}),
    gameProjectId: input.gameProjectId,
    entities,
    facts,
    anchors,
    metadata: {
      lastTurn: input.turn,
      lastSummary: committed?.summary ?? "",
      operationCount: operations.length,
    },
  };
  const viewState = {
    ...(snapshot.viewState && typeof snapshot.viewState === "object"
      ? (snapshot.viewState as Record<string, unknown>)
      : {}),
    player: {
      observations: playerObservations,
    },
  };
  return { worldState, viewState };
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
    intentValidator: checkpoint.intentValidation,
    proposal: checkpoint.proposal,
    committed: checkpoint.committed,
    warnings: checkpoint.warnings,
    narrationText: checkpoint.narrationText ?? "",
  };
}
