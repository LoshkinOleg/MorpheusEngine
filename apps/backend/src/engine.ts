import { randomUUID } from "node:crypto";
import path from "node:path";
import { mkdir, appendFile } from "node:fs/promises";
import type { GameDb } from "./db.js";
import {
  ACTION_CANDIDATES_OUTPUT_CONTRACT,
  type ActionCandidates,
  ActionCandidatesSchema,
  LoremasterOutputSchema,
  PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT,
  ProposedDiffSchema,
  type CommittedDiff,
  type LoremasterOutput,
  type ProposedDiff,
} from "@morpheus/shared";
import type { ChatMessage, LlmProvider } from "./providers/llm.js";

export interface TurnInput {
  runId: string;
  gameProjectId: string;
  turn: number;
  playerInput: string;
  playerId: string;
}

const MAX_JSON_RETRIES = 2;

interface LlmCallAttemptTrace {
  attempt: number;
  requestMessages: ChatMessage[];
  rawResponse?: string;
  error?: string;
}

interface LlmModuleConversationTrace {
  attempts: LlmCallAttemptTrace[];
  usedFallback: boolean;
  fallbackReason?: string;
}

function buildSkippedConversation(stageName: string, reason: string): LlmModuleConversationTrace {
  return {
    attempts: [
      {
        attempt: 1,
        requestMessages: [
          {
            role: "system",
            content: `${stageName} skipped: ${reason}`,
          },
        ],
      },
    ],
    usedFallback: true,
    fallbackReason: reason,
  };
}

function buildFallbackIntent(input: TurnInput) {
  return ActionCandidatesSchema.parse({
    rawInput: input.playerInput,
    candidates: [
      {
        actorId: input.playerId,
        intent: "freeform_action",
        confidence: 0.9,
        params: {
          text: input.playerInput,
        },
      },
    ],
  });
}

function deriveClarificationQuestion(intent: ActionCandidates): string | null {
  const candidateNeedingClarification = intent.candidates.find((candidate) => {
    const tags = candidate.consequenceTags ?? [];
    return tags.includes("needs_clarification") || tags.includes("no_target_in_scope");
  });

  if (!candidateNeedingClarification) {
    return null;
  }

  const question = candidateNeedingClarification.clarificationQuestion?.trim();
  if (question) {
    return question;
  }

  if ((candidateNeedingClarification.consequenceTags ?? []).includes("no_target_in_scope")) {
    if (candidateNeedingClarification.intent === "attack") {
      return "What do you want to attack?";
    }

    return `What do you want to ${candidateNeedingClarification.intent.replace(/_/g, " ")}?`;
  }

  return "Can you clarify what you want to do?";
}

function extractCandidateTarget(params: Record<string, unknown>): string | null {
  const targetKeys = ["target", "targetId", "targetText"];
  for (const key of targetKeys) {
    const value = params[key];
    if (typeof value === "string" && value.trim().length > 0) {
      return value.trim();
    }
  }

  return null;
}

function runLoremasterPlaceholder(
  input: TurnInput,
  intent: ActionCandidates,
): { value: LoremasterOutput; warnings: string[]; conversation: LlmModuleConversationTrace } {
  const assessments = intent.candidates.map((candidate, index) => {
    const tags = [...(candidate.consequenceTags ?? [])];
    const target = extractCandidateTarget(candidate.params ?? {});
    let clarificationQuestion = candidate.clarificationQuestion?.trim();

    if (candidate.intent === "attack" && !target) {
      if (!tags.includes("needs_clarification")) tags.push("needs_clarification");
      if (!tags.includes("no_target_in_scope")) tags.push("no_target_in_scope");
      clarificationQuestion = clarificationQuestion || "What do you want to attack?";
    }

    if (tags.includes("needs_clarification") || tags.includes("no_target_in_scope")) {
      return {
        candidateIndex: index,
        status: "needs_clarification" as const,
        consequenceTags: tags,
        ...(clarificationQuestion ? { clarificationQuestion } : {}),
        rationale: "Clarification required before safe and fair simulation.",
      };
    }

    if (tags.length > 0) {
      return {
        candidateIndex: index,
        status: "allowed_with_consequences" as const,
        consequenceTags: tags,
        rationale: "Action is plausible with explicit consequence handling tags.",
      };
    }

    return {
      candidateIndex: index,
      status: "allowed" as const,
      consequenceTags: [],
      rationale: "No additional constraints detected by placeholder loremaster.",
    };
  });

  return {
    value: LoremasterOutputSchema.parse({
      assessments,
      summary: "Placeholder loremaster assessment completed.",
    }),
    warnings: ["loremaster: placeholder logic active"],
    conversation: buildSkippedConversation(
      "loremaster",
      "Placeholder deterministic evaluator active; no LLM call made.",
    ),
  };
}

function deriveClarificationQuestionFromLoremaster(
  intent: ActionCandidates,
  loremaster: LoremasterOutput,
): string | null {
  const clarificationAssessment = loremaster.assessments.find(
    (assessment) => assessment.status === "needs_clarification",
  );

  if (!clarificationAssessment) {
    return null;
  }

  const explicitQuestion = clarificationAssessment.clarificationQuestion?.trim();
  if (explicitQuestion) {
    return explicitQuestion;
  }

  const candidate = intent.candidates[clarificationAssessment.candidateIndex];
  if (!candidate) {
    return "Can you clarify what you want to do?";
  }

  const fallback = deriveClarificationQuestion({
    candidates: [candidate],
    rawInput: intent.rawInput,
  });
  return fallback ?? "Can you clarify what you want to do?";
}

function buildFallbackProposal(input: TurnInput): ProposedDiff {
  return ProposedDiffSchema.parse({
    moduleName: "default_simulator",
    operations: [
      {
        op: "observation",
        scope: "view:player",
        payload: {
          observerId: input.playerId,
          text: "The desert wind shifts. Your crew waits for your command.",
          turn: input.turn,
        },
        reason: "baseline scene continuity",
      },
    ],
  });
}

const ProposedDiffOperationsOnlySchema = ProposedDiffSchema.pick({
  operations: true,
});

function parseJsonObject(text: string): unknown {
  const trimmed = text.trim();
  const fencedMatch = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  const candidate = fencedMatch ? fencedMatch[1] : trimmed;
  return JSON.parse(candidate);
}

async function generateStructuredWithRetries<T>({
  provider,
  stageName,
  schema,
  basePrompt,
  fallbackValue,
}: {
  provider: LlmProvider;
  stageName: string;
  schema: { parse(input: unknown): T };
  basePrompt: string;
  fallbackValue: T;
}): Promise<{ value: T; warnings: string[]; conversation: LlmModuleConversationTrace }> {
  const warnings: string[] = [];
  let retryHint = "";
  const attempts: LlmCallAttemptTrace[] = [];

  for (let attempt = 0; attempt <= MAX_JSON_RETRIES; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content:
          "Return ONLY valid JSON. Do not add markdown, comments, or extra keys unless requested.",
      },
      {
        role: "user",
        content: `${basePrompt}${retryHint}`,
      },
    ];

    try {
      const raw = await provider.chat(requestMessages);
      const parsed = parseJsonObject(raw);
      const value = schema.parse(parsed);
      attempts.push({
        attempt: attempt + 1,
        requestMessages,
        rawResponse: raw,
      });
      return {
        value,
        warnings,
        conversation: {
          attempts,
          usedFallback: false,
        },
      };
    } catch (error) {
      const errorText = error instanceof Error ? error.message : String(error);
      attempts.push({
        attempt: attempt + 1,
        requestMessages,
        error: errorText,
      });
      if (attempt >= MAX_JSON_RETRIES) {
        warnings.push(`${stageName}: used fallback after retries (${errorText})`);
        return {
          value: fallbackValue,
          warnings,
          conversation: {
            attempts,
            usedFallback: true,
            fallbackReason: errorText,
          },
        };
      }
      retryHint =
        `\n\nYour previous output was invalid.\n` +
        `Fix and return ONLY a valid JSON object for this task.\n` +
        `Validation/parsing error: ${errorText}`;
    }
  }

  warnings.push(`${stageName}: unexpected retry loop exit, used fallback.`);
  return {
    value: fallbackValue,
    warnings,
    conversation: {
      attempts,
      usedFallback: true,
      fallbackReason: "Unexpected retry loop exit.",
    },
  };
}

async function extractIntent(provider: LlmProvider, input: TurnInput) {
  const basePrompt = [
    "Task: convert player input into ActionCandidates JSON.",
    ACTION_CANDIDATES_OUTPUT_CONTRACT,
    "Rules:",
    "- confidence must be between 0 and 1",
    "- include at least one candidate",
    "- keep intent concise snake_case",
    "- add consequenceTags when constraints/side-effects apply",
    "- add consequenceTags: ['needs_clarification'] for ambiguous commands",
    "- include clarificationQuestion when asking for disambiguation",
    "Context:",
    JSON.stringify({
      playerText: input.playerInput,
      playerId: input.playerId,
      turn: input.turn,
    }),
  ].join("\n");

  return generateStructuredWithRetries({
    provider,
    stageName: "intent_extractor",
    schema: ActionCandidatesSchema,
    basePrompt,
    fallbackValue: buildFallbackIntent(input),
  });
}

async function runDefaultSimulator(
  provider: LlmProvider,
  input: TurnInput,
  intent: unknown,
  loremaster: unknown,
) {
  const basePrompt = [
    "Task: produce only the operations array for a soft narrative outcome.",
    PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT,
    "Rules:",
    "- operations can be empty but should usually include at least one observation",
    "- keep outcomes plausible and conservative",
    "Context:",
    JSON.stringify({
      playerInput: input.playerInput,
      playerId: input.playerId,
      turn: input.turn,
      intent,
      loremaster,
    }),
  ].join("\n");

  const fallbackOperationsOnly = ProposedDiffOperationsOnlySchema.parse({
    operations: buildFallbackProposal(input).operations,
  });

  const result = await generateStructuredWithRetries({
    provider,
    stageName: "default_simulator",
    schema: ProposedDiffOperationsOnlySchema,
    basePrompt,
    fallbackValue: fallbackOperationsOnly,
  });

  return {
    value: ProposedDiffSchema.parse({
      moduleName: "default_simulator",
      operations: result.value.operations,
    }),
    warnings: result.warnings,
    conversation: result.conversation,
  };
}

function arbiterCommit(turn: number, proposal: ProposedDiff): CommittedDiff {
  return {
    turn,
    operations: proposal.operations,
    summary: "Action resolved with soft narrative simulation.",
  };
}

function buildClarificationProposal(input: TurnInput, question: string): ProposedDiff {
  return ProposedDiffSchema.parse({
    moduleName: "clarification_handler",
    operations: [
      {
        op: "observation",
        scope: "view:player",
        payload: {
          observerId: input.playerId,
          text: question,
          turn: input.turn,
        },
        reason: "clarification required before safe simulation",
      },
    ],
  });
}

async function generateNarration(
  provider: LlmProvider,
  input: TurnInput,
  committed: CommittedDiff,
): Promise<{ narrationText: string; warnings: string[]; conversation: LlmModuleConversationTrace }> {
  const deriveFallbackNarration = () => {
    const playerObservation = committed.operations.find((operation) => {
      if (operation.op !== "observation") return false;
      const payload = operation.payload as { text?: unknown };
      return typeof payload?.text === "string" && payload.text.trim().length > 0;
    });

    if (playerObservation) {
      const payload = playerObservation.payload as { text: string };
      return payload.text.trim();
    }

    return "The crew acknowledges your order and prepares to act.";
  };
  const fallbackNarration = deriveFallbackNarration();
  const attempts: LlmCallAttemptTrace[] = [];

  const sanitizeNarrationForPlayer = (rawText: string): string => {
    const text = rawText.trim();
    if (!text) return fallbackNarration;

    // Prevent internal trace/JSON leakage into player-facing prose.
    const looksLikeLeak =
      text.startsWith("Stub response to:") ||
      text.includes("committedDiff") ||
      text.includes("\"operations\"") ||
      text.includes("\"playerInput\"") ||
      (text.includes("{") && text.includes("}"));

    if (looksLikeLeak) return fallbackNarration;
    return text;
  };

  if (provider.name === "stub") {
    return {
      narrationText: fallbackNarration,
      warnings: ["proser: stub provider fallback narration"],
      conversation: {
        attempts: [
          {
            attempt: 1,
            requestMessages: [
              {
                role: "system",
                content: "Stub provider active; narration generated from deterministic fallback.",
              },
            ],
          },
        ],
        usedFallback: true,
        fallbackReason: "LLM provider is stub.",
      },
    };
  }

  try {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content:
          "You are the narrative proser. Write 2-4 grounded sentences. Do not invent state outside provided committed diff.",
      },
      {
        role: "user",
        content: JSON.stringify({
          playerInput: input.playerInput,
          turn: input.turn,
          committedDiff: committed,
        }),
      },
    ];
    const rawNarration = await provider.chat(requestMessages);
    attempts.push({
      attempt: 1,
      requestMessages,
      rawResponse: rawNarration,
    });
    return {
      narrationText: sanitizeNarrationForPlayer(rawNarration),
      warnings: [],
      conversation: {
        attempts,
        usedFallback: false,
      },
    };
  } catch (error) {
    const errorText = error instanceof Error ? error.message : String(error);
    attempts.push({
      attempt: 1,
      requestMessages: [
        {
          role: "system",
          content:
            "You are the narrative proser. Write 2-4 grounded sentences. Do not invent state outside provided committed diff.",
        },
        {
          role: "user",
          content: JSON.stringify({
            playerInput: input.playerInput,
            turn: input.turn,
            committedDiff: committed,
          }),
        },
      ],
      error: errorText,
    });
    return {
      narrationText: fallbackNarration,
      warnings: [`proser: used fallback narration (${errorText})`],
      conversation: {
        attempts,
        usedFallback: true,
        fallbackReason: errorText,
      },
    };
  }
}

function resolveSavedDir(gameProjectId: string, sessionId: string): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects", gameProjectId, "saved", sessionId);
}

async function appendTurnLogs(params: {
  gameProjectId: string;
  runId: string;
  turn: number;
  playerInput: string;
  narrationText: string;
  debugPayload: unknown;
}) {
  const savedDir = resolveSavedDir(params.gameProjectId, params.runId);
  await mkdir(savedDir, { recursive: true });

  const normalizedPlayerInput = params.playerInput.replace(/\r?\n/g, "\\n");
  const normalizedNarration = params.narrationText.replace(/\r?\n/g, "\\n");
  const proseEntry =
    `--- turn ${params.turn} ---\n` +
    `You: ${normalizedPlayerInput}\n` +
    `Engine: ${normalizedNarration}\n\n`;

  const debugEntry = JSON.stringify({
    timestamp: new Date().toISOString(),
    turn: params.turn,
    trace: params.debugPayload,
  });

  await appendFile(path.join(savedDir, "prose.log"), proseEntry, "utf8");
  await appendFile(path.join(savedDir, "debug.log"), `${debugEntry}\n`, "utf8");
}

export async function processTurn(db: GameDb, input: TurnInput, llmProvider: LlmProvider) {
  const eventId = randomUUID();
  const intentResult = await extractIntent(llmProvider, input);
  const intent = intentResult.value;
  const loremasterResult = runLoremasterPlaceholder(input, intent);
  const clarificationQuestion = deriveClarificationQuestionFromLoremaster(intent, loremasterResult.value);

  let proposal: ProposedDiff;
  let proposalConversation: LlmModuleConversationTrace;
  let proposalWarnings: string[] = [];

  let narrationText: string;
  let narrationConversation: LlmModuleConversationTrace;
  let narrationWarnings: string[] = [];

  if (clarificationQuestion) {
    proposal = buildClarificationProposal(input, clarificationQuestion);
    proposalConversation = buildSkippedConversation(
      "default_simulator",
      "Clarification required from player before simulation.",
    );
    proposalWarnings = ["default_simulator: skipped because clarification is required"];

    narrationText = clarificationQuestion;
    narrationConversation = buildSkippedConversation(
      "proser",
      "Clarification question emitted directly from engine policy.",
    );
  } else {
    const proposalResult = await runDefaultSimulator(llmProvider, input, intent, loremasterResult.value);
    proposal = proposalResult.value;
    proposalConversation = proposalResult.conversation;
    proposalWarnings = proposalResult.warnings;

    const committedForNarration = arbiterCommit(input.turn, proposal);
    const narrationResult = await generateNarration(llmProvider, input, committedForNarration);
    narrationText = narrationResult.narrationText;
    narrationConversation = narrationResult.conversation;
    narrationWarnings = narrationResult.warnings;
  }

  const warnings = [
    ...intentResult.warnings,
    ...loremasterResult.warnings,
    ...proposalWarnings,
    ...narrationWarnings,
  ];
  const committed = arbiterCommit(input.turn, proposal);
  const turnTracePayload = {
    intent,
    loremaster: loremasterResult.value,
    proposal,
    committed,
    warnings,
    narrationText,
    llmConversations: {
      intent_extractor: intentResult.conversation,
      loremaster: loremasterResult.conversation,
      default_simulator: proposalConversation,
      proser: narrationConversation,
    },
  };

  // Log files are part of the turn output contract.
  // If this fails, we fail the turn instead of silently dropping player-visible/debug history.
  await appendTurnLogs({
    gameProjectId: input.gameProjectId,
    runId: input.runId,
    turn: input.turn,
    playerInput: input.playerInput,
    narrationText,
    debugPayload: turnTracePayload,
  });

  await db.run(
    `INSERT INTO events (run_id, turn, event_type, payload) VALUES (?, ?, ?, ?)`,
    input.runId,
    input.turn,
    "player_input",
    JSON.stringify({ id: eventId, text: input.playerInput }),
  );

  await db.run(
    `INSERT INTO events (run_id, turn, event_type, payload) VALUES (?, ?, ?, ?)`,
    input.runId,
    input.turn,
    "module_trace",
    JSON.stringify(turnTracePayload),
  );

  await db.run(
    `INSERT INTO events (run_id, turn, event_type, payload) VALUES (?, ?, ?, ?)`,
    input.runId,
    input.turn,
    "committed_diff",
    JSON.stringify(committed),
  );

  await db.run(
    `INSERT INTO snapshots (run_id, turn, world_state, view_state) VALUES (?, ?, ?, ?)`,
    input.runId,
    input.turn,
    JSON.stringify({ lastSummary: committed.summary }),
    JSON.stringify({ lastObservation: committed.operations }),
  );

  return {
    intent,
    loremaster: loremasterResult.value,
    proposal,
    committed,
    warnings,
    narrationText,
  };
}
