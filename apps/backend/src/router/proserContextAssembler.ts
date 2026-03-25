import { createHash } from "node:crypto";
import type { GameDb } from "../db.js";
import {
  ModuleRunContextSchema,
  ProserContextTraceSummarySchema,
  ProserModuleRequestSchema,
  type ActionCandidates,
  type CommittedDiff,
  type IntentValidatorOutput,
  type LoreAccumulation,
  type ModuleRunContext,
  type NarrativeCapsule,
  type ProserContextTraceSummary,
} from "@morpheus/shared";
import { listAllLoreRows, readLoreRowsBySubjects, readNarrativeCapsule } from "../sessionStore.js";

const MAX_FLAVOR_KEYS = 4;

function mergeDedupeKeys(...lists: string[][]): string[] {
  const set = new Set<string>();
  for (const list of lists) {
    for (const key of list) {
      const trimmed = key.trim();
      if (trimmed.length > 0) set.add(trimmed);
    }
  }
  return [...set];
}

function stringifyParams(params: Record<string, unknown> | undefined): string {
  if (!params || typeof params !== "object") return "";
  try {
    return JSON.stringify(params).toLowerCase();
  } catch {
    return "";
  }
}

function buildAssemblerKeywords(
  intent: ActionCandidates,
  committed: CommittedDiff,
  intentValidation: IntentValidatorOutput,
): string[] {
  const raw = intent.rawInput
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length >= 3);
  const fromIntents = intent.candidates.flatMap((c) => {
    const intentWord = c.intent.toLowerCase();
    const paramText = stringifyParams(c.params);
    return [intentWord, ...paramText.replace(/[^a-z0-9\s]/g, " ").split(/\s+/).filter((w) => w.length >= 3)];
  });
  const fromRationale = intentValidation.rationale
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length >= 3);
  const fromOps = committed.operations.flatMap((op) => {
    const reason = typeof op.reason === "string" ? op.reason : "";
    const payload =
      op.payload && typeof op.payload === "object"
        ? JSON.stringify(op.payload).toLowerCase()
        : "";
    const blob = `${reason} ${payload}`.replace(/[^a-z0-9\s]/g, " ");
    return blob.split(/\s+/).filter((w) => w.length >= 3);
  });
  return [...new Set([...raw, ...fromIntents, ...fromOps])];
}

function scoreLoreRow(subject: string, data: string, keywords: string[]): number {
  const normalized = `${subject} ${data}`.toLowerCase();
  let score = 0;
  for (const keyword of keywords) {
    if (normalized.includes(keyword)) score += 2;
  }
  if (normalized.includes("desert")) score += 1;
  if (normalized.includes("crawler")) score += 1;
  return score;
}

function narrativeCapsuleFingerprint(capsule: NarrativeCapsule | null | undefined): string | undefined {
  if (!capsule) return undefined;
  return createHash("sha256").update(JSON.stringify(capsule)).digest("hex").slice(0, 16);
}

export interface AssembleProserContextInput {
  db: GameDb;
  context: ModuleRunContext;
  loreAccumulation: LoreAccumulation;
  intent: ActionCandidates;
  intentValidation: IntentValidatorOutput;
  committed: CommittedDiff;
}

export interface AssembleProserContextResult {
  request: ReturnType<typeof ProserModuleRequestSchema.parse>;
  traceSummary: ProserContextTraceSummary;
}

export async function assembleProserContext(input: AssembleProserContextInput): Promise<AssembleProserContextResult> {
  const context = ModuleRunContextSchema.parse(input.context);
  const decisionKeys = mergeDedupeKeys(input.loreAccumulation.loreKeys ?? []);
  const decisionExcerptsRaw = await readLoreRowsBySubjects(input.db, decisionKeys);
  const decisionExcerpts = decisionExcerptsRaw.map((row) => ({
    subject: row.subject.trim(),
    data: row.data.trim(),
    source: row.source.trim(),
  }));

  const keywords = buildAssemblerKeywords(input.intent, input.committed, input.intentValidation);
  const allRows = await listAllLoreRows(input.db);
  const decisionSet = new Set(decisionKeys);
  const scored = allRows
    .filter((row) => !decisionSet.has(row.subject.trim()))
    .map((row) => ({
      row,
      score: scoreLoreRow(row.subject, row.data, keywords),
    }))
    .filter((item) => item.score > 0)
    .sort((a, b) => b.score - a.score)
    .slice(0, MAX_FLAVOR_KEYS);

  const flavorKeys = scored.map((s) => s.row.subject.trim());
  const flavorExcerpts = scored.map((s) => ({
    subject: s.row.subject.trim(),
    data: s.row.data.trim(),
    source: (s.row.source ?? "lore").trim() || "lore",
  }));

  const narrativeCapsule = await readNarrativeCapsule(input.db);

  const request = ProserModuleRequestSchema.parse({
    context,
    committed: input.committed,
    validatedIntent: {
      candidates: input.intent.candidates,
    },
    lore: {
      decisionKeys,
      decisionExcerpts,
      flavorKeys,
      flavorExcerpts,
    },
    ...(narrativeCapsule ? { narrativeCapsule } : {}),
  });

  const traceSummary = ProserContextTraceSummarySchema.parse({
    accumulatedLoreKeyCount: decisionKeys.length,
    decisionExcerptCount: decisionExcerpts.length,
    flavorExcerptCount: flavorExcerpts.length,
    narrativeCapsulePresent: narrativeCapsule !== null,
    narrativeCapsuleHash: narrativeCapsuleFingerprint(narrativeCapsule),
  });

  return { request, traceSummary };
}
