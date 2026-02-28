export interface ActionCandidateContract {
  actorId: string;
  intent: string;
  confidence?: number;
  params?: Record<string, unknown>;
  consequenceTags?: Array<
    | "no_target_in_scope"
    | "partial_success_only"
    | "high_risk_exposure"
    | "resource_cost_applies"
    | "social_backlash"
    | "noise_generated"
  >;
}

export interface ActionCandidatesContract {
  candidates: ActionCandidateContract[];
  rawInput: string;
}

export interface LoremasterAssessmentContract {
  candidateIndex: number;
  status: "allowed" | "allowed_with_consequences";
  consequenceTags: NonNullable<ActionCandidateContract["consequenceTags"]>;
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
