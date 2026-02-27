# LLM Todo (Current)

## P0 - Must do next

- [ ] Implement `TurnBlackboard` shared contract and wire it through `apps/backend/src/router/orchestrator.ts`.
- [ ] Add module telemetry fields to structured traces: parse failures, retries, fallbacks, latency, token usage.
- [ ] Decide whether to keep lore retrieval inside `module-loremaster` or split into standalone `module-lore-retriever`.
- [ ] Add deterministic authoritative gate: reject-only typed errors for deterministic module outputs (no creative retry path).

## P1 - Should do soon

- [ ] Add visibility/fog-of-war enforcement checks so narration cannot leak hidden state.
- [ ] Add contract tests for blackboard, retrieval provenance, and clarification tags.
- [ ] Add endpoint tests for clarification and deterministic rejection branches.
- [ ] Improve Debug UI with module-grouped traces and retry/fallback badges.

## P2 - Can follow after stabilization

- [ ] Add memory tiers (`working`, `episodic`, `semantic`) and retrieval policy per module.
- [ ] Add citation rendering for narration grounding in Debug UI.
- [ ] Add optional ReAct-segmented traces (`thought/action/observation`) for supported modules.
