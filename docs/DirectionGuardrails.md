# Direction Guardrails

Use this file to quickly detect implementation drift from the intended architecture.

## Non-negotiable direction checks

- **LLMs propose; engine commits**
  - No player-visible truth should come from uncommitted model output.
  - Check: `apps/backend/src/router/orchestrator.ts` commits and persists diffs before player-facing state is returned.

- **Structured IR for non-prose modules**
  - Intent and simulation outputs must be schema-validated JSON.
  - Check: `ActionCandidatesSchema`, `ProposedDiffSchema` in `packages/shared/src/schemas.ts`

- **Invariant ownership stays in code**
  - Fixed fields (for example resolver module identity) must be set by code, not model instructions.
  - Check: simulator service stamps `moduleName` as `default_simulator` after model operations parse.

- **Session truth comes from per-run DB**
  - Frontend views must reflect persisted run DB state, not synthetic in-memory-only state.
  - Check: `apps/frontend/src/App.tsx` uses `/run/:runId/state` sync path.

- **Debuggability first**
  - Model prompts, responses, retries, and fallback reasons are visible in Debug UI.
  - Check: `llmConversations` in persisted `module_trace` events and Debug UI sections in `App.tsx`.

## Quick anti-drift checklist before merging major changes

- [ ] Does any new feature bypass schema validation for non-prose model outputs?
- [ ] Did we introduce a model-written invariant that should be code-stamped?
- [ ] Can the player/debug UI diverge from persisted `module_trace` + snapshot state?
- [ ] Are authoritative module boundaries preserved (accept/rerun vs rewrite policy)?
- [ ] Are retry/fallback paths explicit and visible in warnings?

## Common drift patterns to avoid

- Prompt-only contracts with no shared schema enforcement.
- Adding hidden state mutations that are not logged to turn traces.
- UI convenience placeholders that look like real game output.
- Expanding module responsibilities without updating docs/contracts together.
