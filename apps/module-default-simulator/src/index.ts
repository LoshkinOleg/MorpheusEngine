import dotenv from "dotenv";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import {
  PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT,
  ProposedDiffSchema,
  SimulatorModuleRequestSchema,
  SimulatorModuleResponseSchema,
  type ProposedDiff,
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

const port = Number(process.env.MODULE_DEFAULT_SIMULATOR_PORT ?? 8793);
const MAX_JSON_RETRIES = 2;
const ProposedDiffOperationsOnlySchema = ProposedDiffSchema.pick({ operations: true });

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

function buildFallbackProposal(playerId: string, turn: number): ProposedDiff {
  return ProposedDiffSchema.parse({
    moduleName: "default_simulator",
    operations: [
      {
        op: "observation",
        scope: "view:player",
        payload: {
          observerId: playerId,
          text: "The desert wind shifts. Your crew waits for your command.",
          turn,
        },
        reason: "baseline scene continuity",
      },
    ],
  });
}

async function generateProposal(params: {
  playerInput: string;
  playerId: string;
  turn: number;
  intent: unknown;
  lore: unknown;
  loremasterPre: unknown;
}): Promise<{ output: ProposedDiff; warnings: string[]; conversation: ConversationTrace }> {
  const fallbackValue = ProposedDiffOperationsOnlySchema.parse({
    operations: buildFallbackProposal(params.playerId, params.turn).operations,
  });
  const warnings: string[] = [];
  const attempts: ConversationTrace["attempts"] = [];
  if (getProviderName() === "stub") {
    warnings.push("default_simulator: structured generation skipped because LLM provider is stub.");
    return {
      output: ProposedDiffSchema.parse({
        moduleName: "default_simulator",
        operations: fallbackValue.operations,
      }),
      warnings,
      conversation: {
        attempts: [
          {
            attempt: 1,
            requestMessages: [
              {
                role: "system",
                content:
                  "default_simulator skipped: structured generation disabled because provider is stub.",
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
    "Task: produce only the operations array for a soft narrative outcome.",
    PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT,
    "Rules:",
    "- operations can be empty but should usually include at least one observation",
    "- when using observation, prefer payload.text with explicit player-visible result instead of only symbolic event ids",
    "- keep outcomes plausible and conservative",
    "Context:",
    JSON.stringify({
      playerInput: params.playerInput,
      playerId: params.playerId,
      turn: params.turn,
      intent: params.intent,
      loremaster: params.loremasterPre,
      loreEvidence: params.lore,
    }),
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
      const output = ProposedDiffOperationsOnlySchema.parse(parsed);
      attempts.push({ attempt: attempt + 1, requestMessages, rawResponse: raw });
      return {
        output: ProposedDiffSchema.parse({
          moduleName: "default_simulator",
          operations: output.operations,
        }),
        warnings,
        conversation: { attempts, usedFallback: false },
      };
    } catch (error) {
      const errorText = error instanceof Error ? error.message : String(error);
      attempts.push({ attempt: attempt + 1, requestMessages, error: errorText });
      if (attempt >= MAX_JSON_RETRIES) {
        warnings.push(`default_simulator: used fallback after retries (${errorText})`);
        return {
          output: ProposedDiffSchema.parse({
            moduleName: "default_simulator",
            operations: fallbackValue.operations,
          }),
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

  warnings.push("default_simulator: unexpected retry loop exit, used fallback.");
  return {
    output: ProposedDiffSchema.parse({
      moduleName: "default_simulator",
      operations: fallbackValue.operations,
    }),
    warnings,
    conversation: { attempts, usedFallback: true, fallbackReason: "Unexpected retry loop exit." },
  };
}

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "default_simulator", provider: getProviderName() });
});

app.post("/invoke", async (req, res) => {
  try {
    const payload = SimulatorModuleRequestSchema.parse(req.body);
    const result = await generateProposal({
      playerInput: payload.context.playerInput,
      playerId: payload.context.playerId,
      turn: payload.context.turn,
      intent: payload.intent,
      lore: payload.lore,
      loremasterPre: payload.loremasterPre,
    });
    res.json(
      SimulatorModuleResponseSchema.parse({
        meta: { moduleName: "default_simulator", warnings: result.warnings },
        output: result.output,
        debug: { llmConversation: result.conversation },
      }),
    );
  } catch (error) {
    res.status(400).json({ error: error instanceof Error ? error.message : String(error) });
  }
});

app.listen(port, () => {
  console.log(`Morpheus module-default-simulator listening on http://localhost:${port}`);
});
