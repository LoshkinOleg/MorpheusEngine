# Direction Guardrails

Use this file to quickly detect implementation drift from the intended architecture.

## Non-negotiable direction checks

- **LLMs propose; engine commits**
  - No player-visible truth should come from uncommitted model output.
  - Check: `apps/backend/src/engine.ts` uses committed diff for fallback narration source.

- **Structured IR for non-prose modules**
  - Intent and simulation outputs must be schema-validated JSON.
  - Check: `ActionCandidatesSchema`, `ProposedDiffSchema` in `packages/shared/src/schemas.ts`

- **Invariant ownership stays in code**
  - Fixed fields (for example resolver module identity) must be set by code, not model instructions.
  - Check: `runDefaultSimulator()` stamps `moduleName` in code after model operations parse.

- **Session truth comes from files**
  - Frontend views must reflect persisted logs, not synthetic in-memory-only state.
  - Check: `apps/frontend/src/App.tsx` uses `/run/:runId/logs` sync path.

- **Debuggability first**
  - Model prompts, responses, retries, and fallback reasons are visible in Debug UI.
  - Check: `llmConversations` in `debug.log` and Debug UI sections in `App.tsx`.

## Quick anti-drift checklist before merging major changes

- [ ] Does any new feature bypass schema validation for non-prose model outputs?
- [ ] Did we introduce a model-written invariant that should be code-stamped?
- [ ] Can the player UI diverge from `prose.log`?
- [ ] Can the debug UI diverge from `debug.log`?
- [ ] Are authoritative module boundaries preserved (accept/rerun vs rewrite policy)?
- [ ] Are retry/fallback paths explicit and visible in warnings?

## Common drift patterns to avoid

- Prompt-only contracts with no shared schema enforcement.
- Adding hidden state mutations that are not logged to turn traces.
- UI convenience placeholders that look like real game output.
- Expanding module responsibilities without updating docs/contracts together.
