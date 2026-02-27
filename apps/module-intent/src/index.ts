import dotenv from "dotenv";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import {
  ACTION_CANDIDATES_OUTPUT_CONTRACT,
  ActionCandidatesSchema,
  IntentModuleRequestSchema,
  IntentModuleResponseSchema,
  type ActionCandidates,
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

const port = Number(process.env.MODULE_INTENT_PORT ?? 8791);
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

function buildFallbackIntent(playerInput: string, playerId: string): ActionCandidates {
  return ActionCandidatesSchema.parse({
    rawInput: playerInput,
    candidates: [
      {
        actorId: playerId,
        intent: "freeform_action",
        confidence: 0.8,
        params: { text: playerInput },
        consequenceTags: [],
      },
    ],
  });
}

function normalizeIntentOutput(parsed: unknown): unknown {
  if (!parsed || typeof parsed !== "object") return parsed;
  const root = structuredClone(parsed) as { candidates?: Array<Record<string, unknown>> };
  if (!Array.isArray(root.candidates)) return root;
  root.candidates = root.candidates.map((candidate) => {
    if (!candidate || typeof candidate !== "object") return candidate;
    const next = { ...candidate };
    const clarification = next.clarificationQuestion;
    if (typeof clarification === "string" && clarification.trim().length === 0) {
      delete next.clarificationQuestion;
    }
    return next;
  });
  return root;
}

async function generateIntent(playerInput: string, playerId: string, turn: number): Promise<{
  output: ActionCandidates;
  warnings: string[];
  conversation: ConversationTrace;
}> {
  const fallbackValue = buildFallbackIntent(playerInput, playerId);
  const warnings: string[] = [];
  const attempts: ConversationTrace["attempts"] = [];
  if (getProviderName() === "stub") {
    warnings.push("intent_extractor: structured generation skipped because LLM provider is stub.");
    return {
      output: fallbackValue,
      warnings,
      conversation: {
        attempts: [
          {
            attempt: 1,
            requestMessages: [
              {
                role: "system",
                content:
                  "intent_extractor skipped: structured generation disabled because provider is stub.",
              },
            ],
          },
        ],
        usedFallback: true,
        fallbackReason: "LLM provider is stub.",
      },
    };
  }

  let retryHint = "";
  const basePrompt = [
    "Task: convert player input into ActionCandidates JSON.",
    ACTION_CANDIDATES_OUTPUT_CONTRACT,
    "Rules:",
    "- confidence must be between 0 and 1",
    "- include at least one candidate",
    "- keep intent concise snake_case",
    "- add consequenceTags when constraints/side-effects apply",
    "- add consequenceTags: ['needs_clarification'] for ambiguous commands",
    "- include clarificationQuestion only when non-empty and needed",
    "Context:",
    JSON.stringify({ playerText: playerInput, playerId, turn }),
  ].join("\n");

  for (let attempt = 0; attempt <= MAX_JSON_RETRIES; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content: "Return ONLY valid JSON. Do not add markdown, comments, or extra keys unless requested.",
      },
      {
        role: "user",
        content: `${basePrompt}${retryHint}`,
      },
    ];

    try {
      const raw = await chat(requestMessages);
      const parsed = parseJsonObject(raw);
      const normalized = normalizeIntentOutput(parsed);
      const output = ActionCandidatesSchema.parse(normalized);
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
        warnings.push(`intent_extractor: used fallback after retries (${errorText})`);
        return {
          output: fallbackValue,
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

  warnings.push("intent_extractor: unexpected retry loop exit, used fallback.");
  return {
    output: fallbackValue,
    warnings,
    conversation: { attempts, usedFallback: true, fallbackReason: "Unexpected retry loop exit." },
  };
}

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "intent_extractor", provider: getProviderName() });
});

app.post("/invoke", async (req, res) => {
  try {
    const payload = IntentModuleRequestSchema.parse(req.body);
    const { output, warnings, conversation } = await generateIntent(
      payload.context.playerInput,
      payload.context.playerId,
      payload.context.turn,
    );
    const response = IntentModuleResponseSchema.parse({
      meta: {
        moduleName: "intent_extractor",
        warnings,
      },
      output,
      debug: {
        llmConversation: conversation,
      },
    });
    res.json(response);
  } catch (error) {
    res.status(400).json({
      error: error instanceof Error ? error.message : String(error),
    });
  }
});

app.listen(port, () => {
  console.log(`Morpheus module-intent listening on http://localhost:${port}`);
});
