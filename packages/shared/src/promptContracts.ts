import { z } from "zod";
import { ActionCandidatesSchema, LoremasterOutputSchema, ProposedDiffSchema } from "./schemas.js";
import type {
  ActionCandidatesContract,
  LoremasterOutputContract,
  ProposedDiffContract,
} from "./interfaces.js";

function buildOutputContractInstructions(label: string, schema: z.ZodTypeAny): string {
  const jsonSchema = z.toJSONSchema(schema);
  return [
    `Output contract: ${label}`,
    "Return ONLY one JSON object that matches this JSON Schema exactly:",
    "```json",
    JSON.stringify(jsonSchema, null, 2),
    "```",
  ].join("\n");
}

/**
 * JSON Schema generated from `ActionCandidatesSchema`.
 * @see ActionCandidatesContract
 */
export const ActionCandidatesJsonSchema: Record<string, unknown> = z.toJSONSchema(
  ActionCandidatesSchema,
) as Record<string, unknown>;

/**
 * JSON Schema generated from `ProposedDiffSchema`.
 * @see ProposedDiffContract
 */
export const ProposedDiffJsonSchema: Record<string, unknown> = z.toJSONSchema(
  ProposedDiffSchema,
) as Record<string, unknown>;

export const ACTION_CANDIDATES_OUTPUT_CONTRACT = buildOutputContractInstructions(
  "ActionCandidates",
  ActionCandidatesSchema,
);

export const PROPOSED_DIFF_OUTPUT_CONTRACT = buildOutputContractInstructions(
  "ProposedDiff",
  ProposedDiffSchema,
);

/**
 * JSON Schema generated from `LoremasterOutputSchema`.
 * @see LoremasterOutputContract
 */
export const LoremasterOutputJsonSchema: Record<string, unknown> = z.toJSONSchema(
  LoremasterOutputSchema,
) as Record<string, unknown>;

export const LOREMASTER_OUTPUT_CONTRACT = buildOutputContractInstructions(
  "LoremasterOutput",
  LoremasterOutputSchema,
);

const ProposedDiffOperationsOnlySchema = ProposedDiffSchema.pick({
  operations: true,
});

/**
 * JSON Schema generated from `ProposedDiffSchema.pick({ operations: true })`.
 */
export const ProposedDiffOperationsOnlyJsonSchema: Record<string, unknown> = z.toJSONSchema(
  ProposedDiffOperationsOnlySchema,
) as Record<string, unknown>;

export const PROPOSED_DIFF_OPERATIONS_OUTPUT_CONTRACT = buildOutputContractInstructions(
  "ProposedDiff operations payload",
  ProposedDiffOperationsOnlySchema,
);
