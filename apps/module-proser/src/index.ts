import dotenv from "dotenv";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import { z } from "zod";
import {
  GroundedNarrationDraftSchema,
  ProserModuleRequestSchema,
  ProserModuleResponseSchema,
  validateGroundedNarrationAgainstOperations,
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

const port = Number(process.env.MODULE_PROSER_PORT ?? 8794);
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
  if (!response.ok) throw new Error(`Ollama HTTP ${response.status}: ${await response.text()}`);
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

function renderNarrationFromDraft(draft: z.infer<typeof GroundedNarrationDraftSchema>): string {
  return draft.sentences
    .map((sentence) => sentence.text.trim())
    .filter((sentence) => sentence.length > 0)
    .join(" ");
}

function deriveFallbackNarration(committed: unknown): string {
  if (!committed || typeof committed !== "object") {
    return "The crew acknowledges your order and prepares to act.";
  }
  const operations = (committed as { operations?: Array<{ op?: unknown; payload?: unknown }> }).operations;
  const observation = Array.isArray(operations)
    ? operations.find((op) => {
        if (!op || op.op !== "observation") return false;
        const payload = op.payload as { text?: unknown };
        return typeof payload?.text === "string" && payload.text.trim().length > 0;
      })
    : undefined;
  if (observation) {
    const payload = observation.payload as { text: string };
    return payload.text.trim();
  }
  return "The crew acknowledges your order and prepares to act.";
}

function sanitizeNarrationForPlayer(rawText: string, fallbackNarration: string): string {
  const text = rawText.trim();
  if (!text) return fallbackNarration;
  const looksLikeLeak =
    text.startsWith("Stub response to:") ||
    text.includes("committedDiff") ||
    text.includes("\"operations\"") ||
    text.includes("\"playerInput\"") ||
    (text.includes("{") && text.includes("}"));
  if (looksLikeLeak) return fallbackNarration;
  return text;
}

async function generateNarration(payload: Parameters<typeof ProserModuleRequestSchema.parse>[0]) {
  const parsed = ProserModuleRequestSchema.parse(payload);
  const fallbackNarration = deriveFallbackNarration(parsed.committed);
  const attempts: ConversationTrace["attempts"] = [];
  if (getProviderName() === "stub") {
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

  const outputContract = [
    "Return ONLY one JSON object matching this schema:",
    "```json",
    JSON.stringify(z.toJSONSchema(GroundedNarrationDraftSchema), null, 2),
    "```",
  ].join("\n");

  let retryHint = "";
  for (let attempt = 0; attempt <= MAX_JSON_RETRIES; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content:
          "You are the narrative proser. Produce grounded narration draft JSON only. Narrate only facts present in committed operations. Do not add causal consequences unless explicitly present in operation payload/reason text.",
      },
      {
        role: "user",
        content: `${[
          "Task: write 1-4 short grounded narration sentences.",
          outputContract,
          "Rules:",
          "- each sentence must cite one or more operationIndexes it is derived from",
          "- do not introduce new entities, damage, slowdown, injuries, or resource changes unless explicitly in committed operations",
          "- if operations are sparse, keep narration minimal and uncertain rather than speculative",
          "- keep each sentence concise and player-facing",
          "Input:",
          JSON.stringify({
            turn: parsed.context.turn,
            playerId: parsed.context.playerId,
            playerInput: parsed.context.playerInput,
            committedOperations: parsed.committed.operations,
            styleGuidance: {
              mustInclude: parsed.lorePost.mustInclude,
              mustAvoid: parsed.lorePost.mustAvoid,
            },
          }),
          retryHint,
        ].join("\n")}`,
      },
    ];
    try {
      const raw = await chat(requestMessages);
      const parsedDraft = GroundedNarrationDraftSchema.parse(parseJsonObject(raw));
      validateGroundedNarrationAgainstOperations(parsedDraft, parsed.committed);
      attempts.push({ attempt: attempt + 1, requestMessages, rawResponse: raw });
      const narrationText = renderNarrationFromDraft(parsedDraft);
      return {
        narrationText: sanitizeNarrationForPlayer(narrationText, fallbackNarration),
        warnings: [],
        conversation: { attempts, usedFallback: false },
      };
    } catch (error) {
      const errorText = error instanceof Error ? error.message : String(error);
      attempts.push({ attempt: attempt + 1, requestMessages, error: errorText });
      if (attempt >= MAX_JSON_RETRIES) {
        return {
          narrationText: fallbackNarration,
          warnings: [`proser: used fallback narration (${errorText})`],
          conversation: { attempts, usedFallback: true, fallbackReason: errorText },
        };
      }
      retryHint =
        `\n\nYour previous output was invalid.\n` +
        `Fix and return ONLY valid grounded JSON.\n` +
        `Validation/parsing error: ${errorText}`;
    }
  }

  return {
    narrationText: fallbackNarration,
    warnings: ["proser: unexpected retry loop exit."],
    conversation: { attempts, usedFallback: true, fallbackReason: "Unexpected retry loop exit." },
  };
}

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "proser", provider: getProviderName() });
});

app.post("/invoke", async (req, res) => {
  try {
    const result = await generateNarration(req.body);
    res.json(
      ProserModuleResponseSchema.parse({
        meta: {
          moduleName: "proser",
          warnings: result.warnings,
        },
        output: { narrationText: result.narrationText },
        debug: { llmConversation: result.conversation },
      }),
    );
  } catch (error) {
    res.status(400).json({ error: error instanceof Error ? error.message : String(error) });
  }
});

app.listen(port, () => {
  console.log(`Morpheus module-proser listening on http://localhost:${port}`);
});
