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

export type ActionCandidates = z.infer<typeof ActionCandidatesSchema>;
export type LoremasterOutput = z.infer<typeof LoremasterOutputSchema>;
export type ProposedDiff = z.infer<typeof ProposedDiffSchema>;
export type CommittedDiff = z.infer<typeof CommittedDiffSchema>;
export type ObservationEvent = z.infer<typeof ObservationEventSchema>;
