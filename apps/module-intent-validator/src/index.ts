import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import sqlite3 from "sqlite3";
import { open } from "sqlite";
import {
  IntentValidatorModuleRequestSchema,
  IntentValidatorModuleResponseSchema,
  IntentValidatorOutputSchema,
  generateStructuredWithRetries,
  getProviderName,
  loadBackendEnv,
  type ConversationTrace,
  type IntentValidatorOutput,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_INTENT_VALIDATOR_PORT ?? 8796);

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
loadBackendEnv(moduleDir);

function hasNonEmptyTarget(params: Record<string, unknown> | undefined): boolean {
  const invalidPlaceholders = new Set([
    "unknown",
    "none",
    "unspecified",
    "enemy",
    "target",
    "something",
    "someone",
    "anything",
    "it",
    "nothing",
  ]);
  if (!params || typeof params !== "object") return false;
  for (const key of ["target", "subject", "object", "destination"]) {
    const value = params[key];
    if (typeof value === "string") {
      const normalized = value.trim().toLowerCase();
      if (normalized.length > 0 && !invalidPlaceholders.has(normalized)) return true;
    }
  }
  return false;
}

function fallbackValidation(input: {
  intentCandidates: Array<{ intent: string; params?: Record<string, unknown> }>;
}): IntentValidatorOutput {
  const primary = input.intentCandidates[0];
  if (!primary) {
    return IntentValidatorOutputSchema.parse({
      decision: "refuse",
      rationale: "No intent candidate available for validation.",
      refusalReason: "Refused: no actionable intent candidate was produced.",
      consequenceConstraints: {
        requiredOutcomes: [],
        forbiddenOutcomes: [],
      },
      loreKeysUsed: [],
    });
  }

  if (primary.intent === "attack" && !hasNonEmptyTarget(primary.params)) {
    return IntentValidatorOutputSchema.parse({
      decision: "refuse",
      rationale: "Attack intent requires a concrete target.",
      refusalReason: "Refused: no valid attack target is currently in scope.",
      consequenceConstraints: {
        requiredOutcomes: [],
        forbiddenOutcomes: [],
      },
      loreKeysUsed: [],
    });
  }

  return IntentValidatorOutputSchema.parse({
    decision: "accept",
    rationale: "Intent is accepted under deterministic fallback policy.",
    consequenceConstraints: {
      requiredOutcomes: ["The action is accepted and should produce a grounded observable result."],
      forbiddenOutcomes: [],
    },
    loreKeysUsed: [],
  });
}

function normalizeOutcomeProse(value: string): string {
  const trimmed = value.trim();
  if (trimmed.length === 0) return "";
  const looksSymbolic =
    /[.:_]/.test(trimmed) ||
    /^[a-z0-9_.:-]+$/.test(trimmed) ||
    !/[a-zA-Z]/.test(trimmed.replace(/[\s-]/g, ""));
  if (!looksSymbolic) return trimmed;
  const naturalized = trimmed
    .replace(/[._:]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  if (naturalized.length === 0) return "";
  const sentence = `Ensure ${naturalized}.`;
  return sentence.charAt(0).toUpperCase() + sentence.slice(1);
}

function normalizeValidatorOutput(parsed: unknown): unknown {
  if (!parsed || typeof parsed !== "object") return parsed;
  const next = structuredClone(parsed) as Record<string, unknown>;
  if (typeof next.refusalReason === "string" && next.refusalReason.trim().length === 0) {
    delete next.refusalReason;
  }
  if (!next.consequenceConstraints || typeof next.consequenceConstraints !== "object") {
    next.consequenceConstraints = {
      requiredOutcomes: [],
      forbiddenOutcomes: [],
    };
  } else {
    const constraints = next.consequenceConstraints as Record<string, unknown>;
    const normalizeList = (value: unknown): string[] => {
      if (!Array.isArray(value)) return [];
      return value
        .filter((item): item is string => typeof item === "string")
        .map(normalizeOutcomeProse)
        .filter((item) => item.length > 0);
    };
    constraints.requiredOutcomes = normalizeList(constraints.requiredOutcomes);
    constraints.forbiddenOutcomes = normalizeList(constraints.forbiddenOutcomes);
    next.consequenceConstraints = constraints;
  }
  if (!Array.isArray(next.loreKeysUsed)) {
    next.loreKeysUsed = [];
  } else {
    next.loreKeysUsed = next.loreKeysUsed
      .filter((key): key is string => typeof key === "string")
      .map((key) => key.trim())
      .filter((key) => key.length > 0);
  }
  return next;
}

function enforceDeterministicRefusal(
  output: IntentValidatorOutput,
  intentCandidates: Array<{ intent: string; params?: Record<string, unknown> }>,
): IntentValidatorOutput {
  const primary = intentCandidates[0];
  if (!primary) return output;
  if (primary.intent === "attack" && !hasNonEmptyTarget(primary.params)) {
    return IntentValidatorOutputSchema.parse({
      decision: "refuse",
      rationale: "Attack intent requires a concrete in-scope target.",
      refusalReason: "Refused: no valid attack target is currently in scope.",
      consequenceConstraints: {
        requiredOutcomes: [],
        forbiddenOutcomes: [],
      },
      loreKeysUsed: output.loreKeysUsed ?? [],
    });
  }
  return output;
}

function resolveRunDbPath(gameProjectId: string, runId: string): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects", gameProjectId, "saved", runId, "world_state.db");
}

function buildLoreKeywordSet(
  playerInput: string,
  intentCandidates: Array<{ intent: string; params?: Record<string, unknown> }>,
): string[] {
  const words = playerInput
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length >= 3);
  const intents = intentCandidates.map((candidate) => candidate.intent.toLowerCase());
  const paramBlobs = intentCandidates.flatMap((candidate) => {
    if (!candidate.params || typeof candidate.params !== "object") return [];
    try {
      return JSON.stringify(candidate.params).toLowerCase();
    } catch {
      return "";
    }
  });
  const paramWords = paramBlobs
    .join(" ")
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length >= 3);
  return [...new Set([...words, ...intents, ...paramWords])];
}

function scoreLoreSubject(subject: string, data: string, keywords: string[]): number {
  const normalized = `${subject} ${data}`.toLowerCase();
  let score = 0;
  for (const keyword of keywords) {
    if (normalized.includes(keyword)) score += 2;
  }
  if (normalized.includes("desert")) score += 1;
  if (normalized.includes("crawler")) score += 1;
  return score;
}

async function resolveLoreSubjectsUsed(
  gameProjectId: string,
  runId: string,
  playerInput: string,
  intentCandidates: Array<{ intent: string; params?: Record<string, unknown> }>,
): Promise<string[]> {
  const dbPath = resolveRunDbPath(gameProjectId, runId);
  if (!fs.existsSync(dbPath)) return [];
  const keywords = buildLoreKeywordSet(playerInput, intentCandidates);
  if (keywords.length === 0) return [];

  let db: Awaited<ReturnType<typeof open>> | null = null;
  try {
    db = await open({
      filename: dbPath,
      driver: sqlite3.Database,
    });
    const rows = await db.all<Array<{ subject: string; data: string }>>(`SELECT subject, data FROM lore`);
    const scored = rows
      .filter((row) => row.subject?.trim() && row.data?.trim())
      .map((row) => ({
        subject: row.subject.trim(),
        score: scoreLoreSubject(row.subject, row.data, keywords),
      }))
      .filter((row) => row.score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, 4);
    return scored.map((row) => row.subject);
  } catch {
    return [];
  } finally {
    if (db) await db.close();
  }
}

async function validateIntent(params: {
  gameProjectId: string;
  runId: string;
  playerInput: string;
  playerId: string;
  turn: number;
  intentCandidates: Array<{ actorId: string; intent: string; params?: Record<string, unknown> }>;
}): Promise<{ output: IntentValidatorOutput; warnings: string[]; conversation: ConversationTrace }> {
  const fallbackValue = fallbackValidation({
    intentCandidates: params.intentCandidates,
  });

  const basePrompt = [
    "Task: validate extracted intent candidates against game reality and produce structured constraints.",
    "Output contract: IntentValidatorOutput",
    "Rules:",
    "- return decision='refuse' when action cannot be grounded safely",
    "- include refusalReason only for decision='refuse'",
    "- if accepted, provide consequenceConstraints with realistic requiredOutcomes/forbiddenOutcomes",
    "- requiredOutcomes and forbiddenOutcomes must be short natural-language prose statements, not symbolic ids",
    "- keep rationale concise and concrete",
    "- loreKeysUsed: optional array of lore.subject keys you relied on (may be empty); router merges with DB-scored keys",
    "Context:",
    JSON.stringify({
      playerId: params.playerId,
      turn: params.turn,
      intentCandidates: params.intentCandidates,
    }),
  ].join("\n");

  const result = await generateStructuredWithRetries({
    stageName: "intent_validator",
    schema: IntentValidatorOutputSchema,
    basePrompt,
    fallbackValue,
    normalizeParsed: normalizeValidatorOutput,
  });

  const enforced = enforceDeterministicRefusal(result.output, params.intentCandidates);
  const dbLoreSubjects = await resolveLoreSubjectsUsed(
    params.gameProjectId,
    params.runId,
    params.playerInput,
    params.intentCandidates,
  );
  const mergedKeys = [...new Set([...(enforced.loreKeysUsed ?? []), ...dbLoreSubjects])];
  return {
    ...result,
    output: IntentValidatorOutputSchema.parse({
      ...enforced,
      loreKeysUsed: mergedKeys,
    }),
  };
}

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "intent_validator", provider: getProviderName() });
});

app.post("/invoke", async (req, res) => {
  try {
    const payload = IntentValidatorModuleRequestSchema.parse(req.body);
    const { output, warnings, conversation } = await validateIntent({
      gameProjectId: payload.context.gameProjectId,
      runId: payload.context.runId,
      playerInput: payload.context.playerInput,
      playerId: payload.context.playerId,
      turn: payload.context.turn,
      intentCandidates: payload.intent.candidates,
    });
    res.json(
      IntentValidatorModuleResponseSchema.parse({
        meta: {
          moduleName: "intent_validator",
          warnings,
        },
        output,
        debug: {
          llmConversation: conversation,
        },
      }),
    );
  } catch (error) {
    res.status(400).json({
      error: error instanceof Error ? error.message : String(error),
    });
  }
});

app.listen(port, () => {
  console.log(`Morpheus module-intent-validator listening on http://localhost:${port}`);
});
