import { z } from "zod";

export const StabilitySchema = z.enum(["ephemeral", "session", "canon"]);

export const EntitySchema = z.object({
  id: z.string().min(1),
  kind: z.string().min(1),
  name: z.string().optional(),
  tags: z.array(z.string()).default([]),
  links: z.array(z.string()).default([]),
});

export const FactSchema = z.object({
  subjectId: z.string().min(1),
  key: z.string().min(1),
  value: z.unknown(),
  confidence: z.number().min(0).max(1).default(1),
  source: z.string().min(1),
  stability: StabilitySchema.default("session"),
});

export const ActionCandidateSchema = z.object({
  actorId: z.string().min(1),
  intent: z.string().min(1),
  confidence: z.number().min(0).max(1).default(0.5),
  params: z.record(z.string(), z.unknown()).default({}),
  consequenceTags: z
    .array(
      z.enum([
        "needs_clarification",
        "no_target_in_scope",
        "partial_success_only",
        "high_risk_exposure",
        "resource_cost_applies",
        "social_backlash",
        "noise_generated",
      ]),
    )
    .default([]),
  clarificationQuestion: z.string().min(1).optional(),
});

export const ActionCandidatesSchema = z.object({
  candidates: z.array(ActionCandidateSchema).min(1),
  rawInput: z.string().min(1),
});

export const LoremasterAssessmentSchema = z.object({
  candidateIndex: z.number().int().nonnegative(),
  status: z.enum(["allowed", "allowed_with_consequences", "needs_clarification"]),
  consequenceTags: ActionCandidateSchema.shape.consequenceTags.default([]),
  clarificationQuestion: z.string().min(1).optional(),
  rationale: z.string().min(1),
});

export const LoremasterOutputSchema = z.object({
  assessments: z.array(LoremasterAssessmentSchema).min(1),
  summary: z.string().min(1),
});

export const LoreEvidenceItemSchema = z.object({
  source: z.string().min(1),
  excerpt: z.string().min(1),
  score: z.number(),
});

export const LoreRetrievalSchema = z.object({
  query: z.string().min(1),
  evidence: z.array(LoreEvidenceItemSchema),
  summary: z.string().min(1),
});

export const LoremasterPostOutputSchema = z.object({
  status: z.enum(["consistent", "needs_adjustment"]),
  rationale: z.string().min(1),
  mustInclude: z.array(z.string().min(1)).default([]),
  mustAvoid: z.array(z.string().min(1)).default([]),
});

export const StateWriteScopeSchema = z.enum(["world", "view:player"]);

export const ProposedDiffOperationSchema = z.object({
  op: z.enum(["upsert_fact", "remove_fact", "upsert_entity", "observation", "detection"]),
  scope: StateWriteScopeSchema.default("world"),
  payload: z.record(z.string(), z.unknown()),
  reason: z.string().min(1),
});

export const ProposedDiffSchema = z.object({
  moduleName: z.string().min(1),
  operations: z.array(ProposedDiffOperationSchema),
});

export const CommittedDiffSchema = z.object({
  turn: z.number().int().nonnegative(),
  operations: z.array(ProposedDiffOperationSchema),
  summary: z.string().min(1),
});

export const ObservationEventSchema = z.object({
  observerId: z.string().min(1),
  text: z.string().min(1),
  turn: z.number().int().nonnegative(),
});

export const ModuleRunContextSchema = z.object({
  requestId: z.string().min(1),
  runId: z.string().min(1),
  gameProjectId: z.string().min(1),
  turn: z.number().int().nonnegative(),
  playerId: z.string().min(1),
  playerInput: z.string(),
});

export const ModuleEnvelopeMetaSchema = z.object({
  moduleName: z.string().min(1),
  warnings: z.array(z.string()).default([]),
});

export const IntentModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
});

export const IntentModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: ActionCandidatesSchema,
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const LoreRetrieverModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  intent: ActionCandidatesSchema,
});

export const LoreRetrieverModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: LoreRetrievalSchema,
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const LoremasterPreModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  intent: ActionCandidatesSchema,
  lore: LoreRetrievalSchema,
});

export const LoremasterPreModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: LoremasterOutputSchema,
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const LoremasterPostModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  intent: ActionCandidatesSchema,
  lore: LoreRetrievalSchema,
  proposal: ProposedDiffSchema,
});

export const LoremasterPostModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: LoremasterPostOutputSchema,
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const SimulatorModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  intent: ActionCandidatesSchema,
  loremasterPre: LoremasterOutputSchema,
  lore: LoreRetrievalSchema,
});

export const SimulatorModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: ProposedDiffSchema,
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const ArbiterModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  intent: ActionCandidatesSchema,
  lore: LoreRetrievalSchema,
  loremasterPre: LoremasterOutputSchema,
  proposal: ProposedDiffSchema,
  lorePost: LoremasterPostOutputSchema,
});

export const ArbiterDecisionSchema = z.enum(["accept", "request_rerun", "choose_alternative"]);

export const ArbiterModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: z.object({
    decision: ArbiterDecisionSchema,
    selectedProposal: ProposedDiffSchema,
    rationale: z.string().min(1),
    rerunHints: z.array(z.string()).default([]),
    selectionMetadata: z.record(z.string(), z.unknown()).default({}),
  }),
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const ProserModuleRequestSchema = z.object({
  context: ModuleRunContextSchema,
  committed: CommittedDiffSchema,
  lore: LoreRetrievalSchema,
  lorePost: LoremasterPostOutputSchema,
});

export const ProserModuleResponseSchema = z.object({
  meta: ModuleEnvelopeMetaSchema,
  output: z.object({
    narrationText: z.string().min(1),
  }),
  debug: z.record(z.string(), z.unknown()).default({}),
});

export const PipelineStepStatusSchema = z.enum(["ok", "error", "skipped"]);

export const PipelineStepEventSchema = z.object({
  stepNumber: z.number().int().positive(),
  stage: z.string().min(1),
  endpoint: z.string().min(1),
  status: PipelineStepStatusSchema,
  request: z.unknown(),
  response: z.unknown().optional(),
  warnings: z.array(z.string()).default([]),
  error: z.string().min(1).optional(),
  startedAt: z.string().min(1),
  finishedAt: z.string().min(1),
});

export const TurnExecutionModeSchema = z.enum(["normal", "step"]);

export const TurnExecutionStateSchema = z.object({
  runId: z.string().min(1),
  turn: z.number().int().positive(),
  mode: TurnExecutionModeSchema,
  cursor: z.number().int().nonnegative(),
  completed: z.boolean(),
  createdAt: z.string().min(1),
  updatedAt: z.string().min(1),
  playerInput: z.string(),
  playerId: z.string().min(1),
  requestId: z.string().min(1),
  gameProjectId: z.string().min(1),
  result: z
    .object({
      narrationText: z.string().optional(),
      warnings: z.array(z.string()).default([]),
    })
    .default({ warnings: [] }),
});

export type ActionCandidates = z.infer<typeof ActionCandidatesSchema>;
export type LoremasterOutput = z.infer<typeof LoremasterOutputSchema>;
export type LoreRetrieval = z.infer<typeof LoreRetrievalSchema>;
export type LoreEvidenceItem = z.infer<typeof LoreEvidenceItemSchema>;
export type LoremasterPostOutput = z.infer<typeof LoremasterPostOutputSchema>;
export type ProposedDiff = z.infer<typeof ProposedDiffSchema>;
export type CommittedDiff = z.infer<typeof CommittedDiffSchema>;
export type ObservationEvent = z.infer<typeof ObservationEventSchema>;
export type ModuleRunContext = z.infer<typeof ModuleRunContextSchema>;
export type PipelineStepEvent = z.infer<typeof PipelineStepEventSchema>;
export type PipelineStepStatus = z.infer<typeof PipelineStepStatusSchema>;
export type TurnExecutionState = z.infer<typeof TurnExecutionStateSchema>;
export type TurnExecutionMode = z.infer<typeof TurnExecutionModeSchema>;
export type ArbiterDecision = z.infer<typeof ArbiterDecisionSchema>;
