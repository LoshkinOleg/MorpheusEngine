import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import { z } from "zod";
import {
  GroundedNarrationDraftSchema,
  ProserModuleRequestSchema,
  ProserModuleResponseSchema,
  chatWithProvider,
  getProviderName,
  loadBackendEnv,
  parseJsonObject,
  type ChatMessage,
  type ConversationTrace,
  validateGroundedNarrationAgainstOperations,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_PROSER_PORT ?? 8794);
const MAX_JSON_RETRIES = 2;

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
loadBackendEnv(moduleDir);

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

  const sensoryContext = {
    decisionLore: parsed.lore.decisionExcerpts,
    flavorLore: parsed.lore.flavorExcerpts,
    narrativeCapsule: parsed.narrativeCapsule ?? null,
  };

  let retryHint = "";
  for (let attempt = 0; attempt <= MAX_JSON_RETRIES; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      {
        role: "system",
        content: [
          "You are the narrative proser. Output grounded narration draft JSON only.",
          "Semantic source of truth: validatedIntent (structured) + committed operations — not raw player chat text.",
          "You may use lore excerpts and narrativeCapsule only for sensory texture, tone, and setting-consistent wording.",
          "Do not invent new plot events, outcomes, or state changes beyond what committed operations encode.",
          "Do not add causal consequences unless explicitly present in operation payload or reason text.",
        ].join(" "),
      },
      {
        role: "user",
        content: `${[
          "Task: write 1-4 short grounded narration sentences for this turn.",
          outputContract,
          "Rules:",
          "- each sentence must cite one or more operationIndexes it is derived from (committed operations list order, 0-based)",
          "- do not introduce new entities, damage, slowdown, injuries, or resource changes unless explicitly in committed operations",
          "- sensory and atmospheric detail is allowed when consistent with lore excerpts below; do not contradict decision lore",
          "- if operations are sparse, keep narration minimal and uncertain rather than speculative",
          "- keep each sentence concise and player-facing",
          "validatedIntent (canonical action semantics):",
          JSON.stringify(parsed.validatedIntent),
          "committedOperations:",
          JSON.stringify(parsed.committed.operations),
          "sensoryAndNarrativeLayers (texture only, not new facts):",
          JSON.stringify(sensoryContext),
          "contextMetadata (ids/turn only; do not treat playerInput as authoritative for what happened):",
          JSON.stringify({
            turn: parsed.context.turn,
            playerId: parsed.context.playerId,
          }),
          retryHint,
        ].join("\n")}`,
      },
    ];
    try {
      const raw = await chatWithProvider(requestMessages);
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
