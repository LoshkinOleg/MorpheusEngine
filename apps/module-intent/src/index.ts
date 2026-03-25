import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import {
  ACTION_CANDIDATES_OUTPUT_CONTRACT,
  ActionCandidatesSchema,
  IntentModuleRequestSchema,
  IntentModuleResponseSchema,
  generateStructuredWithRetries,
  getProviderName,
  loadBackendEnv,
  type ActionCandidates,
  type ConversationTrace,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_INTENT_PORT ?? 8791);

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
loadBackendEnv(moduleDir);

const CANONICAL_INTENTS = [
  "inspect",
  "move_self",
  "wait",
  "attack",
  "move_other",
  "freeform_action",
] as const;
type CanonicalIntent = (typeof CANONICAL_INTENTS)[number];

function normalizeActorId(actorId: string, fallbackPlayerId: string): string {
  const normalized = actorId.trim().replace(/^entity\./, "");
  return normalized.length > 0 ? normalized : fallbackPlayerId;
}

function firstNonEmptyString(value: unknown): string | null {
  if (typeof value !== "string") return null;
  const normalized = value.trim();
  return normalized.length > 0 ? normalized : null;
}

function firstKnownParam(params: Record<string, unknown>, keys: string[]): string | null {
  for (const key of keys) {
    const value = firstNonEmptyString(params[key]);
    if (value) return value;
  }
  return null;
}

function normalizeSubjectText(value: string): string {
  return value
    .replace(/^[\s"'`]+|[\s"'`.,!?;:]+$/g, "")
    .replace(/^(the|a|an)\s+/i, "")
    .trim();
}

function extractByRegexes(rawInput: string, patterns: RegExp[]): string | null {
  for (const pattern of patterns) {
    const match = rawInput.match(pattern);
    if (!match) continue;
    const extracted = normalizeSubjectText(match[1] ?? "");
    if (extracted.length > 0) return extracted;
  }
  return null;
}

function inferCanonicalIntent(intent: string, rawInput: string): CanonicalIntent | null {
  const normalizedIntent = intent.trim().toLowerCase().replace(/\s+/g, "_");
  const normalizedInput = rawInput.trim().toLowerCase();

  if (["inspect", "look_around", "inspect_self", "look_at_surroundings", "examine", "observe"].includes(normalizedIntent)) {
    return "inspect";
  }
  if (["move_self", "move_out", "go_to_location", "go", "move", "travel", "enter_sandcrawler", "enter"].includes(normalizedIntent)) {
    return "move_self";
  }
  if (["wait", "idle", "pause", "pass_turn"].includes(normalizedIntent)) {
    return "wait";
  }
  if (["attack", "kick", "strike", "hit", "punch", "shoot", "stab", "kick_treads"].includes(normalizedIntent)) {
    return "attack";
  }
  if (["move_other", "move_object", "push", "pull", "nudge", "drag", "relocate"].includes(normalizedIntent)) {
    return "move_other";
  }

  if (/^(look|inspect|examine|observe)\b/.test(normalizedInput)) return "inspect";
  if (/^(go|move|walk|travel|head|enter)\b/.test(normalizedInput)) return "move_self";
  if (/^(wait|idle|pause)\b/.test(normalizedInput)) return "wait";
  if (/^(attack|kick|hit|strike|punch|stab|shoot)\b/.test(normalizedInput)) return "attack";
  if (/^(push|pull|drag|nudge|move)\b/.test(normalizedInput) && /\b(the|a|an)\b/.test(normalizedInput)) {
    return "move_other";
  }
  return null;
}

function buildCanonicalParams(
  canonicalIntent: CanonicalIntent,
  rawInput: string,
  sourceParams: Record<string, unknown>,
): Record<string, unknown> {
  if (canonicalIntent === "inspect") {
    const subject =
      firstKnownParam(sourceParams, ["subject", "target", "object", "what"]) ??
      extractByRegexes(rawInput, [
        /\b(?:look|inspect|examine|observe)\s+(?:at\s+)?(?:the\s+|a\s+|an\s+)?([^.,!?]+)$/i,
      ]) ??
      "surroundings";
    return { subject };
  }
  if (canonicalIntent === "move_self") {
    const destination =
      firstKnownParam(sourceParams, ["destination", "target", "location", "to", "where"]) ??
      extractByRegexes(rawInput, [
        /\b(?:to|towards?|into|inside)\s+(?:the\s+|a\s+|an\s+)?([^.,!?]+)$/i,
        /\b(?:go|move|walk|travel|head|enter)\s+(?:to\s+|towards?\s+|into\s+|inside\s+)?(?:the\s+|a\s+|an\s+)?([^.,!?]+)$/i,
      ]) ??
      "forward";
    return { destination };
  }
  if (canonicalIntent === "wait") {
    const duration =
      firstKnownParam(sourceParams, ["duration", "time", "for"]) ??
      extractByRegexes(rawInput, [
        /\bfor\s+([^.,!?]+)$/i,
        /\bwait(?:ing)?\s+([^.,!?]+)$/i,
      ]) ??
      "briefly";
    return { duration };
  }
  if (canonicalIntent === "attack") {
    const target =
      firstKnownParam(sourceParams, ["target", "subject", "object", "victim"]) ??
      extractByRegexes(rawInput, [
        /\b(?:attack|kick|hit|strike|punch|stab|shoot)\s+(?:the\s+|a\s+|an\s+)?([^.,!?]+)$/i,
      ]) ??
      "unknown";
    return { target };
  }
  const object =
    firstKnownParam(sourceParams, ["object", "target", "subject", "thing"]) ??
    extractByRegexes(rawInput, [
      /\b(?:move|push|pull|drag|nudge|shift|relocate)\s+(?:the\s+|a\s+|an\s+)?([^.,!?]+)$/i,
    ]) ??
    "unknown";
  return { object };
}

function normalizeIntentOutput(parsed: unknown, playerId: string): unknown {
  if (!parsed || typeof parsed !== "object") return parsed;
  const root = structuredClone(parsed) as {
    rawInput?: unknown;
    candidates?: Array<Record<string, unknown>>;
  };
  if (!Array.isArray(root.candidates)) return root;

  const rawInput = firstNonEmptyString(root.rawInput) ?? "";
  root.candidates = root.candidates.map((candidate) => {
    if (!candidate || typeof candidate !== "object") return candidate;
    const normalized = { ...candidate };
    const intent = firstNonEmptyString(normalized.intent) ?? "freeform_action";
    const params =
      normalized.params && typeof normalized.params === "object" && !Array.isArray(normalized.params)
        ? (normalized.params as Record<string, unknown>)
        : {};

    normalized.actorId = normalizeActorId(
      firstNonEmptyString(normalized.actorId) ?? playerId,
      playerId,
    );
    if ("confidence" in normalized) {
      delete normalized.confidence;
    }
    if ("consequenceTags" in normalized) {
      delete normalized.consequenceTags;
    }

    const canonicalIntent = inferCanonicalIntent(intent, rawInput);
    if (canonicalIntent) {
      normalized.intent = canonicalIntent;
      normalized.params =
        canonicalIntent === "freeform_action"
          ? params
          : buildCanonicalParams(canonicalIntent, rawInput, params);
      return normalized;
    }

    normalized.intent = "freeform_action";
    normalized.params = params;
    return normalized;
  });
  return root;
}

function buildFallbackIntent(playerInput: string, playerId: string): ActionCandidates {
  return ActionCandidatesSchema.parse({
    rawInput: playerInput,
    candidates: [
      {
        actorId: normalizeActorId(playerId, "player"),
        intent: "freeform_action",
        params: {},
      },
    ],
  });
}

async function generateIntent(playerInput: string, playerId: string, turn: number): Promise<{
  output: ActionCandidates;
  warnings: string[];
  conversation: ConversationTrace;
}> {
  const fallbackValue = buildFallbackIntent(playerInput, playerId);
  const basePrompt = [
    "Task: convert player input into ActionCandidates JSON.",
    ACTION_CANDIDATES_OUTPUT_CONTRACT,
    "Rules:",
    "- include at least one candidate",
    "- keep intent concise snake_case",
    "- when reasonable, map intent to one of: inspect, move_self, wait, attack, move_other",
    "- if no canonical action above reasonably fits, use intent='freeform_action'",
    "- freeform_action does not require any specific params shape",
    "- for inspect, put the inspected subject in params.subject",
    "- for move_self, put the destination in params.destination",
    "- for wait, put the wait duration/time in params.duration",
    "- for attack, put the attack target in params.target",
    "- for move_other, put the moved thing in params.object",
    "- actorId should be a role handle (for example player.captain), not an entity-prefixed id",
    "Context:",
    JSON.stringify({ playerText: playerInput, playerId, turn }),
  ].join("\n");
  const result = await generateStructuredWithRetries({
    stageName: "intent_extractor",
    schema: ActionCandidatesSchema,
    basePrompt,
    fallbackValue,
    normalizeParsed: (parsed) => normalizeIntentOutput(parsed, playerId),
  });
  return result;
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
