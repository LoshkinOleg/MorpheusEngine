export interface ActionCandidateContract {
  actorId: string;
  intent: string;
  params?: Record<string, unknown>;
}

export interface ActionCandidatesContract {
  candidates: ActionCandidateContract[];
  rawInput: string;
}

export interface IntentValidatorConsequenceConstraintsContract {
  requiredOutcomes: string[];
  forbiddenOutcomes: string[];
}

export interface IntentValidatorOutputContract {
  decision: "accept" | "refuse";
  rationale: string;
  refusalReason?: string;
  consequenceConstraints: IntentValidatorConsequenceConstraintsContract;
  /** Lore table primary keys (`lore.subject`) consulted during validation */
  loreKeysUsed?: string[];
}

export interface ProserLoreBundleContract {
  decisionKeys: string[];
  decisionExcerpts: Array<{ subject: string; data: string; source: string }>;
  flavorKeys: string[];
  flavorExcerpts: Array<{ subject: string; data: string; source: string }>;
}

export interface NarrativeCapsuleContract {
  rollingSummary: string;
  tension: "calm" | "rising" | "high";
  lastTurnsDigest?: string;
}

export interface LoremasterAssessmentContract {
  candidateIndex: number;
  status: "allowed" | "allowed_with_consequences";
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
