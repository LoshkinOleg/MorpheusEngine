import path from "node:path";
import fs from "node:fs";
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
  generateStructuredWithRetries,
  getProviderName,
  loadBackendEnv,
  type ActionCandidates,
  type LoremasterOutput,
  type LoreRetrieval,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_LOREMASTER_PORT ?? 8792);

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
loadBackendEnv(moduleDir);

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

function fallbackPre(intent: ActionCandidates): LoremasterOutput {
  const assessments = intent.candidates.map((_candidate, index) => ({
    candidateIndex: index,
    status: "allowed" as const,
    rationale: "No additional constraints detected.",
  }));
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
    if ("consequenceTags" in next) {
      delete next.consequenceTags;
    }
    const status = typeof next.status === "string" ? next.status : "";
    if (status !== "allowed" && status !== "allowed_with_consequences") {
      next.status = "allowed_with_consequences";
      if (typeof next.rationale !== "string" || next.rationale.trim().length === 0) {
        next.rationale = "Action needs tighter consequence framing.";
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
