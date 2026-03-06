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
    "- confidence must be between 0 and 1",
    "- include at least one candidate",
    "- keep intent concise snake_case",
    "- add consequenceTags when constraints/side-effects apply",
    "- if target/scope is invalid, use consequenceTags: ['no_target_in_scope']",
    "Context:",
    JSON.stringify({ playerText: playerInput, playerId, turn }),
  ].join("\n");
  const result = await generateStructuredWithRetries({
    stageName: "intent_extractor",
    schema: ActionCandidatesSchema,
    basePrompt,
    fallbackValue,
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
