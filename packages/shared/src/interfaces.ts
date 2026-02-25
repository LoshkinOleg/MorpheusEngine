export interface ActionCandidateContract {
  actorId: string;
  intent: string;
  confidence?: number;
  params?: Record<string, unknown>;
  consequenceTags?: Array<
    | "needs_clarification"
    | "no_target_in_scope"
    | "partial_success_only"
    | "high_risk_exposure"
    | "resource_cost_applies"
    | "social_backlash"
    | "noise_generated"
  >;
  clarificationQuestion?: string;
}

export interface ActionCandidatesContract {
  candidates: ActionCandidateContract[];
  rawInput: string;
}

export interface LoremasterAssessmentContract {
  candidateIndex: number;
  status: "allowed" | "allowed_with_consequences" | "needs_clarification";
  consequenceTags: NonNullable<ActionCandidateContract["consequenceTags"]>;
  clarificationQuestion?: string;
  rationale: string;
}

export interface LoremasterOutputContract {
  assessments: LoremasterAssessmentContract[];
  summary: string;
}

export interface ProposedDiffOperationContract {
  op: "upsert_fact" | "remove_fact" | "upsert_entity" | "observation" | "detection";
  scope?: "world" | "view:player";
  payload: Record<string, unknown>;
  reason: string;
}

export interface ProposedDiffContract {
  moduleName: string;
  operations: ProposedDiffOperationContract[];
}
