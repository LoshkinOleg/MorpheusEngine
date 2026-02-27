# LLM Implementation Roadmap 0001

This roadmap translates the architecture direction into ticketized implementation units with explicit acceptance signals.

## Ticket set A - Shared schema contracts

- [ ] `SCHEMA-001` Add `TurnBlackboardSchema` in `packages/shared/src/schemas.ts`.
  - Scope:
    - `context`, `candidates`, `constraints`, `proposals`, `committed`, `citations`, `warnings`
  - Accept when:
    - exports compile
    - backend can parse/emit blackboard payload per turn

- [ ] `SCHEMA-002` Add retrieval provenance contracts.
  - Scope:
    - `EvidenceItemSchema`
    - `RetrievalTraceSchema`
  - Accept when:
    - evidence IDs and sources are typed and validated
    - `promptContracts.ts` can render retrieval contract instructions

- [ ] `SCHEMA-003` Add deterministic authoritative gate result contract.
  - Scope:
    - `DeterministicGateResultSchema`
    - reject reasons: `contract_error | invariant_error | input_version_error`
  - Accept when:
    - arbiter branch can emit typed reject result
    - frontend debug payload can display typed reason

## Ticket set B - Backend runtime pipeline

- [ ] `RUNTIME-001` Introduce blackboard runtime object in `apps/backend/src/router/orchestrator.ts`.
  - Scope:
    - create once per turn
    - modules read/write only through blackboard sections
  - Accept when:
    - turn trace includes blackboard snapshot entries
    - existing `/turn` response remains backward compatible

- [ ] `RUNTIME-002` Implement `lore_retriever` scaffold stage.
  - Scope:
    - deterministic placeholder retrieval from game project lore
    - emit retrieval trace with selected evidence IDs
  - Accept when:
    - debug trace contains retrieval stage each turn
    - loremaster/default simulator can consume retrieval payload

- [ ] `RUNTIME-003` Add module telemetry counters and timings.
  - Scope:
    - parse failures, retries, fallbacks, timeout count, latency, token usage
  - Accept when:
    - telemetry is present in debug entries
    - no breaking API contract changes

- [ ] `RUNTIME-004` Enforce deterministic authoritative policy gate.
  - Scope:
    - disable “retry for different outcome” for deterministic resolvers
    - allow reject with typed reason only
  - Accept when:
    - policy is enforced by tests for identical-input deterministic runs

- [ ] `RUNTIME-005` Add visibility leak checks before prose output.
  - Scope:
    - detect hidden facts not present in `ViewState(player)`
    - strip or downgrade leaked claims
  - Accept when:
    - hidden-state regression test fails without guard and passes with guard

## Ticket set C - Frontend debug/inspection

- [ ] `UI-001` Group debug traces by module stage with compact headers.
  - Accept when:
    - intent/loremaster/retrieval/simulator/arbiter/proser sections are clearly separated

- [ ] `UI-002` Add retry/fallback telemetry badges and counters.
  - Accept when:
    - user can see retries/fallbacks without opening raw conversation payloads

- [ ] `UI-003` Add citation panel for retrieval provenance.
  - Accept when:
    - selected evidence IDs and sources are visible for each turn

## Ticket set D - Tests and quality gates

- [ ] `TEST-001` Contract tests for new schemas (`TurnBlackboard`, retrieval, deterministic gate).
- [ ] `TEST-002` Endpoint tests for clarification + deterministic reject branches.
- [ ] `TEST-003` Regression test: narration does not leak hidden world facts.
- [ ] `TEST-004` Snapshot test: debug trace contains required stages and telemetry fields.

## Suggested implementation order

1. `SCHEMA-001`, `SCHEMA-002`, `SCHEMA-003`
2. `RUNTIME-001`, `RUNTIME-002`, `RUNTIME-003`
3. `RUNTIME-004`, `RUNTIME-005`
4. `UI-001`, `UI-002`, `UI-003`
5. `TEST-001` to `TEST-004`

## Notes

- Keep external API payloads backward compatible while adding debug fields.
- Prefer additive contract evolution and deprecation windows.
- Keep deterministic policy enforcement in code, not prompt policy text.
