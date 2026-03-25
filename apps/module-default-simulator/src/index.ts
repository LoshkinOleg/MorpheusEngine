import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import {
  PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT,
  ProposedDiffSchema,
  SimulatorModuleRequestSchema,
  SimulatorModuleResponseSchema,
  generateStructuredWithRetries,
  getProviderName,
  loadBackendEnv,
  type ConversationTrace,
  type ProposedDiff,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_DEFAULT_SIMULATOR_PORT ?? 8793);
const ProposedDiffOperationsOnlySchema = ProposedDiffSchema.pick({ operations: true });

const moduleDir = path.dirname(fileURLToPath(import.meta.url));
loadBackendEnv(moduleDir);

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
  intentValidation: unknown;
}): Promise<{ output: ProposedDiff; warnings: string[]; conversation: ConversationTrace }> {
  const fallbackValue = ProposedDiffOperationsOnlySchema.parse({
    operations: buildFallbackProposal(params.playerId, params.turn).operations,
  });
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
      intentValidation: params.intentValidation,
    }),
  ].join("\n");
  const result = await generateStructuredWithRetries({
    stageName: "default_simulator",
    schema: ProposedDiffOperationsOnlySchema,
    basePrompt,
    fallbackValue,
  });
  return {
    output: ProposedDiffSchema.parse({
      moduleName: "default_simulator",
      operations: result.output.operations,
    }),
    warnings: result.warnings,
    conversation: result.conversation,
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
      intentValidation: payload.intentValidation,
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
