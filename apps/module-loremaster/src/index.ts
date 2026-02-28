import dotenv from "dotenv";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import { z } from "zod";
import sqlite3 from "sqlite3";
import { open } from "sqlite";
import {
  LOREMASTER_OUTPUT_CONTRACT,
  LoreRetrieverModuleRequestSchema,
  LoreRetrieverModuleResponseSchema,
  LoremasterOutputSchema,
  LoremasterPostModuleRequestSchema,
  LoremasterPostModuleResponseSchema,
  LoremasterPostOutputSchema,
  LoremasterPreModuleRequestSchema,
  LoremasterPreModuleResponseSchema,
  type ActionCandidates,
  type LoremasterOutput,
  type LoreRetrieval,
} from "@morpheus/shared";

type ChatMessage = { role: "system" | "user" | "assistant"; content: string };
type ConversationTrace = {
  attempts: Array<{
    attempt: number;
    requestMessages: ChatMessage[];
    rawResponse?: string;
    error?: string;
  }>;
  usedFallback: boolean;
  fallbackReason?: string;
};

const port = Number(process.env.MODULE_LOREMASTER_PORT ?? 8792);
const MAX_JSON_RETRIES = 2;

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
for (const candidate of [
  path.resolve(moduleDir, "..", "..", "backend", ".env"),
  path.resolve(process.cwd(), "apps", "backend", ".env"),
  path.resolve(process.cwd(), ".env"),
  path.resolve(moduleDir, "..", ".env"),
]) {
  if (fs.existsSync(candidate)) {
    dotenv.config({ path: candidate, override: true });
  }
}

function getProviderName(): "ollama" | "stub" {
  return (process.env.LLM_PROVIDER ?? "stub").toLowerCase() === "ollama" ? "ollama" : "stub";
}

async function chat(messages: ChatMessage[]): Promise<string> {
  if (getProviderName() === "stub") {
    const lastUser = [...messages].reverse().find((item) => item.role === "user");
    return `Stub response to: ${lastUser?.content ?? "empty input"}`;
  }

  const baseUrl = process.env.OLLAMA_BASE_URL ?? "http://localhost:11434";
  const model = process.env.OLLAMA_MODEL ?? "qwen2.5:7b-instruct";
  const timeoutMs = Number(process.env.OLLAMA_TIMEOUT_MS ?? "15000");
  const response = await fetch(`${baseUrl.replace(/\/$/, "")}/api/chat`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ model, messages, stream: false }),
    signal: AbortSignal.timeout(Number.isFinite(timeoutMs) && timeoutMs > 0 ? timeoutMs : 15000),
  });
  if (!response.ok) {
    throw new Error(`Ollama HTTP ${response.status}: ${await response.text()}`);
  }
  const data = (await response.json()) as { message?: { content?: string } };
  const content = data.message?.content?.trim();
  if (!content) throw new Error("No content in Ollama response.");
  return content;
}

function parseJsonObject(text: string): unknown {
  const trimmed = text.trim();
  const fencedMatch = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  const candidate = fencedMatch ? fencedMatch[1] : trimmed;
  try {
    return JSON.parse(candidate);
  } catch {
    const start = candidate.indexOf("{");
    if (start < 0) throw new Error("No JSON object found in model response.");
    let depth = 0;
    let inString = false;
    let escaped = false;
    for (let index = start; index < candidate.length; index += 1) {
      const char = candidate[index];
      if (inString) {
        if (escaped) escaped = false;
        else if (char === "\\") escaped = true;
        else if (char === "\"") inString = false;
        continue;
      }
      if (char === "\"") {
        inString = true;
        continue;
      }
      if (char === "{") depth += 1;
      if (char === "}") {
        depth -= 1;
        if (depth === 0) return JSON.parse(candidate.slice(start, index + 1));
      }
    }
    throw new Error("Could not isolate JSON object from model response.");
  }
}

async function generateStructuredWithRetries<T>(params: {
  stageName: string;
  schema: { parse(input: unknown): T };
  basePrompt: string;
  fallbackValue: T;
  normalizeParsed?: (parsed: unknown) => unknown;
}): Promise<{ output: T; warnings: string[]; conversation: ConversationTrace }> {
  const warnings: string[] = [];
  const attempts: ConversationTrace["attempts"] = [];
  if (getProviderName() === "stub") {
    const reason = `${params.stageName}: structured generation skipped because LLM provider is stub.`;
    warnings.push(reason);
    return {
      output: params.fallbackValue,
      warnings,
      conversation: {
        attempts: [
          {
            attempt: 1,
            requestMessages: [{ role: "system", content: `${params.stageName} skipped: ${reason}` }],
          },
        ],
        usedFallback: true,
        fallbackReason: reason,
      },
    };
  }

  let retryHint = "";
  for (let attempt = 0; attempt <= MAX_JSON_RETRIES; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content: "Return ONLY valid JSON. Do not add markdown, comments, or extra keys unless requested.",
      },
      {
        role: "user",
        content: `${params.basePrompt}${retryHint}`,
      },
    ];
    try {
      const raw = await chat(requestMessages);
      const parsed = parseJsonObject(raw);
      const normalized = params.normalizeParsed ? params.normalizeParsed(parsed) : parsed;
      const output = params.schema.parse(normalized);
      attempts.push({ attempt: attempt + 1, requestMessages, rawResponse: raw });
      return {
        output,
        warnings,
        conversation: { attempts, usedFallback: false },
      };
    } catch (error) {
      const errorText = error instanceof Error ? error.message : String(error);
      attempts.push({ attempt: attempt + 1, requestMessages, error: errorText });
      if (attempt >= MAX_JSON_RETRIES) {
        warnings.push(`${params.stageName}: used fallback after retries (${errorText})`);
        return {
          output: params.fallbackValue,
          warnings,
          conversation: { attempts, usedFallback: true, fallbackReason: errorText },
        };
      }
      retryHint =
        `\n\nYour previous output was invalid.\n` +
        `Fix and return ONLY a valid JSON object for this task.\n` +
        `Validation/parsing error: ${errorText}`;
    }
  }

  warnings.push(`${params.stageName}: unexpected retry loop exit, used fallback.`);
  return {
    output: params.fallbackValue,
    warnings,
    conversation: { attempts, usedFallback: true, fallbackReason: "Unexpected retry loop exit." },
  };
}

function resolveRunDbPath(gameProjectId: string, runId: string): string {
  return path.resolve(process.cwd(), "..", "..", "game_projects", gameProjectId, "saved", runId, "world_state.db");
}

function buildKeywordSet(playerInput: string, intent: ActionCandidates): string[] {
  const words = playerInput
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length >= 3);
  const intents = intent.candidates.map((candidate) => candidate.intent.toLowerCase());
  return [...new Set([...words, ...intents])];
}

function scoreLore(subject: string, data: string, keywords: string[]): number {
  const normalized = `${subject} ${data}`.toLowerCase();
  let score = 0;
  for (const keyword of keywords) {
    if (normalized.includes(keyword)) score += 2;
  }
  if (normalized.includes("desert")) score += 1;
  if (normalized.includes("crawler")) score += 1;
  return score;
}

async function retrieveLore(
  gameProjectId: string,
  runId: string,
  playerInput: string,
  intent: ActionCandidates,
): Promise<LoreRetrieval> {
  const dbPath = resolveRunDbPath(gameProjectId, runId);
  const evidence: LoreRetrieval["evidence"] = [];
  const keywords = buildKeywordSet(playerInput, intent);
  if (!fs.existsSync(dbPath)) {
    return {
      query: playerInput,
      evidence: [],
      summary: "No lore DB found for run.",
    };
  }
  let db: Awaited<ReturnType<typeof open>> | null = null;
  try {
    db = await open({
      filename: dbPath,
      driver: sqlite3.Database,
    });
    const rows = await db.all<Array<{ subject: string; data: string; source: string }>>(
      `SELECT subject, data, source FROM lore`,
    );
    for (const row of rows) {
      if (!row.subject?.trim() || !row.data?.trim()) continue;
      evidence.push({
        source: row.source?.trim() || "lore",
        excerpt: `${row.subject.trim()}: ${row.data.trim()}`,
        score: scoreLore(row.subject, row.data, keywords),
      });
    }
  } catch {
    return {
      query: playerInput,
      evidence: [],
      summary: "Lore DB query failed.",
    };
  } finally {
    if (db) await db.close();
  }

  const selected = [...evidence].sort((a, b) => b.score - a.score).slice(0, 4);
  return {
    query: playerInput,
    evidence: selected,
    summary:
      selected.length > 0
        ? `Retrieved ${selected.length} lore rows from lore table.`
        : "No lore rows retrieved from lore table.",
  };
}

function extractCandidateTarget(params: Record<string, unknown>): string | null {
  for (const key of ["target", "targetId", "targetText"]) {
    const value = params[key];
    if (typeof value === "string" && value.trim().length > 0) return value.trim();
  }
  return null;
}

function fallbackPre(intent: ActionCandidates): LoremasterOutput {
  const assessments = intent.candidates.map((candidate, index) => {
    const tags = [...(candidate.consequenceTags ?? [])];
    const target = extractCandidateTarget(candidate.params ?? {});
    if (candidate.intent === "attack" && !target) {
      if (!tags.includes("no_target_in_scope")) tags.push("no_target_in_scope");
    }
    if (tags.length > 0) {
      return {
        candidateIndex: index,
        status: "allowed_with_consequences" as const,
        consequenceTags: tags,
        rationale: tags.includes("no_target_in_scope")
          ? "Action cannot be resolved safely because no valid target is in scope."
          : "Action is plausible with explicit consequence handling.",
      };
    }
    return {
      candidateIndex: index,
      status: "allowed" as const,
      consequenceTags: [],
      rationale: "No additional constraints detected.",
    };
  });
  return LoremasterOutputSchema.parse({
    assessments,
    summary: "Fallback loremaster pre-diff assessment completed.",
  });
}

function normalizePre(parsed: unknown): unknown {
  if (!parsed || typeof parsed !== "object") return parsed;
  const root = structuredClone(parsed) as { assessments?: Array<Record<string, unknown>> };
  if (!Array.isArray(root.assessments)) return root;
  root.assessments = root.assessments.map((assessment) => {
    if (!assessment || typeof assessment !== "object") return assessment;
    const next = { ...assessment };
    const status = typeof next.status === "string" ? next.status : "";
    if (status !== "allowed" && status !== "allowed_with_consequences") {
      next.status = "allowed_with_consequences";
      const rawTags = Array.isArray(next.consequenceTags) ? next.consequenceTags : [];
      next.consequenceTags = rawTags.includes("no_target_in_scope")
        ? rawTags
        : [...rawTags, "no_target_in_scope"];
      if (typeof next.rationale !== "string" || next.rationale.trim().length === 0) {
        next.rationale = "Action cannot be resolved safely because no valid target is in scope.";
      }
    }
    return next;
  });
  return root;
}

function normalizePost(parsed: unknown): unknown {
  if (!parsed || typeof parsed !== "object") return parsed;
  const root = structuredClone(parsed) as { mustInclude?: unknown; mustAvoid?: unknown };
  const normalizeArray = (value: unknown): string[] =>
    Array.isArray(value)
      ? value
          .filter((item): item is string => typeof item === "string")
          .map((item) => item.trim())
          .filter((item) => item.length > 0)
      : [];
  root.mustInclude = normalizeArray(root.mustInclude);
  root.mustAvoid = normalizeArray(root.mustAvoid);
  return root;
}

function fallbackPost() {
  return LoremasterPostOutputSchema.parse({
    status: "consistent",
    rationale: "Fallback post-diff lore check assumes consistency.",
    mustInclude: ["crawler", "desert", "scarcity"],
    mustAvoid: ["ocean", "sails", "rigging", "sea spray"],
  });
}

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "loremaster", provider: getProviderName() });
});

app.post("/retrieve", async (req, res) => {
  try {
    const payload = LoreRetrieverModuleRequestSchema.parse(req.body);
    const output = await retrieveLore(
      payload.context.gameProjectId,
      payload.context.runId,
      payload.context.playerInput,
      payload.intent,
    );
    res.json(
      LoreRetrieverModuleResponseSchema.parse({
        meta: { moduleName: "loremaster.retrieve", warnings: [] },
        output,
        debug: {},
      }),
    );
  } catch (error) {
    res.status(400).json({ error: error instanceof Error ? error.message : String(error) });
  }
});

app.post("/pre", async (req, res) => {
  try {
    const payload = LoremasterPreModuleRequestSchema.parse(req.body);
    const basePrompt = [
      "Task: assess action candidates for plausibility and refusal risk.",
      LOREMASTER_OUTPUT_CONTRACT,
      "Rules:",
      "- return one assessment per candidate index in the same order",
      "- status must be either 'allowed' or 'allowed_with_consequences'",
      "- use consequenceTags (for example 'no_target_in_scope') when refusal justification is required",
      "- keep rationale concise and concrete",
      "Context:",
      JSON.stringify({
        playerInput: payload.context.playerInput,
        playerId: payload.context.playerId,
        turn: payload.context.turn,
        intent: payload.intent,
        loreEvidence: payload.lore.evidence,
      }),
    ].join("\n");

    const result = await generateStructuredWithRetries({
      stageName: "loremaster_pre",
      schema: LoremasterOutputSchema,
      basePrompt,
      fallbackValue: fallbackPre(payload.intent),
      normalizeParsed: normalizePre,
    });

    res.json(
      LoremasterPreModuleResponseSchema.parse({
        meta: {
          moduleName: "loremaster_pre",
          warnings: result.warnings,
        },
        output: result.output,
        debug: { llmConversation: result.conversation },
      }),
    );
  } catch (error) {
    res.status(400).json({ error: error instanceof Error ? error.message : String(error) });
  }
});

app.post("/post", async (req, res) => {
  try {
    const payload = LoremasterPostModuleRequestSchema.parse(req.body);
    const postContractSchemaText = JSON.stringify(z.toJSONSchema(LoremasterPostOutputSchema), null, 2);
    const basePrompt = [
      "Task: validate whether the proposed outcome remains lore-consistent and style-consistent.",
      "Output contract: LoremasterPostOutput",
      "Return ONLY one JSON object that matches this JSON Schema exactly:",
      "```json",
      postContractSchemaText,
      "```",
      "Rules:",
      "- status='needs_adjustment' if outcome or narration direction likely violates setting lore",
      "- keep rationale concise",
      "- mustInclude and mustAvoid should be concrete short phrases",
      "Context:",
      JSON.stringify({
        playerInput: payload.context.playerInput,
        playerId: payload.context.playerId,
        turn: payload.context.turn,
        intent: payload.intent,
        proposal: payload.proposal,
        loreEvidence: payload.lore.evidence,
      }),
    ].join("\n");

    const result = await generateStructuredWithRetries({
      stageName: "loremaster_post",
      schema: LoremasterPostOutputSchema,
      basePrompt,
      fallbackValue: fallbackPost(),
      normalizeParsed: normalizePost,
    });

    res.json(
      LoremasterPostModuleResponseSchema.parse({
        meta: {
          moduleName: "loremaster_post",
          warnings: result.warnings,
        },
        output: result.output,
        debug: { llmConversation: result.conversation },
      }),
    );
  } catch (error) {
    res.status(400).json({ error: error instanceof Error ? error.message : String(error) });
  }
});

app.listen(port, () => {
  console.log(`Morpheus module-loremaster listening on http://localhost:${port}`);
});
