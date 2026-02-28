import { z } from "zod";
import { CommittedDiffSchema, GroundedNarrationDraftSchema } from "./schemas.js";

type GroundedNarrationDraft = z.infer<typeof GroundedNarrationDraftSchema>;
type CommittedDiff = z.infer<typeof CommittedDiffSchema>;

function getOperationSourceText(committed: CommittedDiff): string[] {
  return committed.operations.map((operation) =>
    JSON.stringify({
      op: operation.op,
      scope: operation.scope,
      payload: operation.payload,
      reason: operation.reason,
    }).toLowerCase(),
  );
}

function hasUngroundedConsequence(sentence: string, sourceText: string): boolean {
  const riskyTerms = [
    "damage",
    "damaged",
    "destroy",
    "destroyed",
    "break",
    "broken",
    "slow",
    "slowed",
    "incapacitate",
    "incapacitated",
    "injury",
    "injured",
    "bleed",
    "bleeding",
  ];
  const lowered = sentence.toLowerCase();
  return riskyTerms.some((term) => lowered.includes(term) && !sourceText.includes(term));
}

export function validateGroundedNarrationAgainstOperations(
  draft: GroundedNarrationDraft,
  committed: CommittedDiff,
): void {
  const sourceByIndex = getOperationSourceText(committed);
  const operationCount = sourceByIndex.length;
  for (const sentence of draft.sentences) {
    const uniqueIndexes = [...new Set(sentence.operationIndexes)];
    if (uniqueIndexes.some((idx) => idx < 0 || idx >= operationCount)) {
      throw new Error("Grounded draft references non-existent operation index.");
    }
    const sourceText = uniqueIndexes.map((idx) => sourceByIndex[idx]).join(" ");
    if (hasUngroundedConsequence(sentence.text, sourceText)) {
      throw new Error("Grounded draft introduces consequence not present in committed operations.");
    }
  }
}
