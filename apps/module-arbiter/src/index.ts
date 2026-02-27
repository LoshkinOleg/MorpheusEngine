import dotenv from "dotenv";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import express from "express";
import cors from "cors";
import {
  ArbiterModuleRequestSchema,
  ArbiterModuleResponseSchema,
} from "@morpheus/shared";

const port = Number(process.env.MODULE_ARBITER_PORT ?? 8795);

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

const app = express();
app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({ ok: true, module: "arbiter" });
});

app.post("/invoke", (req, res) => {
  try {
    const payload = ArbiterModuleRequestSchema.parse(req.body);
    const response = ArbiterModuleResponseSchema.parse({
      meta: {
        moduleName: "arbiter",
        warnings: [],
      },
      output: {
        decision: "accept",
        selectedProposal: payload.proposal,
        rationale: "Arbiter accepts validated upstream proposal by current policy.",
        rerunHints: [],
        selectionMetadata: {
          policyMode: "accept_validated_proposal",
        },
      },
      debug: {
        llmConversation: {
          attempts: [
            {
              attempt: 1,
              requestMessages: [
                {
                  role: "system",
                  content: "Arbiter deterministic pass-through mode is active.",
                },
              ],
            },
          ],
          usedFallback: false,
        },
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
  console.log(`Morpheus module-arbiter listening on http://localhost:${port}`);
});
