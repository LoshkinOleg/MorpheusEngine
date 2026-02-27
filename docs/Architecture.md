# Morpheus Engine Architecture

This document describes the target architecture and the current MVP implementation state.

## 1) System topology

The project is a TypeScript monorepo with four runtime layers:

- **Frontend (`apps/frontend`)**
  - React UI for player interaction.
  - Core UX scope: stacked Game UI + Debug UI with session selector and DB-backed state.
- **Backend API + router (`apps/backend`)**
  - Runs API endpoints and router orchestration.
  - Loads game project manifests.
  - Persists events and snapshots.
  - Calls standalone module services over HTTP.
- **Standalone module services (`apps/module-*`)**
  - One process per pipeline module (`intent_extractor`, `loremaster`, `default_simulator`, `arbiter`, `proser`).
  - Expose module-specific invoke endpoints.
- **Shared contracts (`packages/shared`)**
  - Common schema definitions and types for module IR and inter-process request/response contracts.

## 2) Turn loop architecture

The runtime follows an event-sourced turn pattern:

1. **Input capture**
   - Player action is received (`POST /turn`) and stored as an event.
2. **Router pipeline calls**
   - Router calls `intent_extractor`, lore retrieval, `loremaster` pre-check.
3. **Simulation proposals**
   - One or more modules produce `ProposedDiff` operations.
4. **Post-diff lore checks**
   - Router calls `loremaster` post-check for outcome/narration consistency hints.
5. **Arbiter commit**
   - Final operations are selected/merged into `CommittedDiff`.
6. **Persistence**
   - Pipeline step events, module trace, and snapshots are written to storage.
7. **Presentation**
   - UI consumes committed result, trace payloads, and ordered pipeline events from storage.

Why this pattern:

- Enables per-turn debug visibility.
- Makes module behavior auditable.
- Supports adding/replacing modules without rewriting the core loop.

## 3) Module graph (target model)

Planned default pipeline:

1. `intent_extractor` (LLM)
2. `loremaster` retrieval + pre/post checks (standalone service)
3. optional external `lore_retriever` service (future split if needed)
4. resolvers:
   - authoritative code simulators (when available)
   - fallback `default_simulator` (LLM)
5. `arbiter` (standalone service, currently accept-first policy)
6. optional critic loop
7. `proser` (LLM)

Current state:

- Router + standalone module services are implemented for intent, loremaster, default simulation, and prose generation.

Authoritative vs advisory resolver policy:

- **Authoritative modules** (for example stealth/damage): arbiter must not creatively rewrite domain outputs except non-domain metadata.
  - If authoritative module is deterministic/classical code, arbiter may accept or reject for contract/invariant/input-version errors, but must not request "retry for different outcome" on identical inputs.
  - If authoritative module is LLM-based, arbiter may request bounded rerun/correction only for malformed output or policy/constraint violations.
- **Advisory modules** (for example fallback/default simulation): arbiter may edit, merge, or replace outputs as needed.

This preserves fairness for mechanics-heavy packs while keeping narrative flexibility elsewhere.

## 4) World and knowledge model

### 4.1 Core entities

Minimal universal entity fields:

- `id`
- `kind`
- optional `name`
- `tags[]`
- `links[]`

### 4.2 Facts as primary extension mechanism

Non-core data lives in namespaced facts:

- `subjectId`
- `key`
- `value` (JSON)
- `confidence`
- `source`
- `stability`: `ephemeral | session | canon`

This allows progressive simulation growth without pre-authoring every object.

### 4.3 Anchors instead of tile maps

Locations are narrative anchors, not strict grids:

- Anchors provide stable "place buckets" for revisitable context.
- Entities store where they are via anchor reference.
- Anchors are creatable on demand during play (for example a newly added "lounge room"), then reused in later turns for narrative consistency.

### 4.4 Hidden information and fairness

State is conceptually split into:

- `WorldState` (ground truth)
- `ViewState(player)` (what the player has observed)

Event types such as observation and detection are used to make stealth outcomes explainable and debuggable.

### 4.5 Latent entities vs tracked entities

The engine distinguishes between:

- **Latent/background entities**: mentioned in prose or broad simulation context, but not yet fully tracked in state.
- **Tracked entities**: promoted entities with stable IDs, facts, links, and optional anchor placement.

Promotion is an intentional behavior, not an accident. Typical promotion signals include:

- repeated mention across turns
- sustained dialogue with the player
- assignment of a durable role/task
- mechanical relevance (combat, inventory, stealth, economy, crew duties)

Promotion decisions are finalized by arbiter at commit time, then validated by deterministic checks.

## 5) Storage model (MVP)

Backend uses per-run SQLite (`game_projects/<id>/saved/<runId>/world_state.db`) with:

- `events`
  - append-only turn events and module traces.
- `snapshots`
  - persisted world/view snapshots per turn.
- `meta`
  - run/game project metadata for the session DB.
- `turn_execution`
  - in-progress/completed turn execution state for normal and step mode.
- `pipeline_events`
  - ordered per-turn step events (`stepNumber`, stage, endpoint, request/response payloads).

This supports local-first development and turn replay diagnostics (without requiring deterministic replay).

## 6) API layer

Current endpoints:

- `GET /`
- `GET /health`
- `GET /game_projects/:id`
- `GET /game_projects/:id/sessions`
- `POST /run/start`
- `GET /run/:runId/state`
- `GET /run/:runId/turn/:turn/pipeline`
- `POST /run/:runId/open-saved-folder`
- `POST /turn`
- `POST /turn/step/start`
- `POST /turn/step/next`

### 6.1 Endpoint details

#### `GET /health`

Purpose:

- Lightweight backend liveness probe for local dev, frontend boot checks, and deployment health checks.

Request:

- No body.

Success response (`200`):

```json
{
  "ok": true
}
```

Failure behavior:

- Should fail only on severe service startup/runtime issues.

#### `GET /game_projects/:id`

Purpose:

- Fetch the game project manifest for inspection/bootstrapping.

Request:

- Path param `id` (example: `sandcrawler`).

Success response (`200`):

- Returns the raw `manifest.json` for `game_projects/:id`.

Example (`200`):

```json
{
  "id": "sandcrawler",
  "title": "Sandcrawler Captain",
  "engineVersion": "0.1.x",
  "entryState": {
    "playerId": "entity.player.captain",
    "anchorId": "anchor.bridge"
  },
  "moduleBindings": {
    "intent_extractor": "llm.default",
    "lore_retriever": "data.default",
    "loremaster": "llm.default",
    "default_simulator": "llm.default",
    "arbiter": "llm.default",
    "proser": "llm.default"
  }
}
```

Not found (`404`) example:

```json
{
  "error": "Game project not found",
  "details": "<error text>"
}
```

#### `POST /run/start`

Purpose:

- Start a new gameplay run and create turn-0 snapshot state.

Request:

- No body in current MVP.

Behavior:

- Generates a new `runId`.
- Loads configured default game project (`GAME_PROJECT_ID`).
- Writes run metadata and initial snapshot to SQLite.

Success response (`200`) example:

```json
{
  "runId": "4de6c31b-d535-4cf6-b8f4-f248f87f5f4c",
  "gameProject": "sandcrawler"
}
```

Failure behavior:

- `500` if default game project cannot be loaded or persistence fails.

#### `GET /game_projects/:id/sessions`

Purpose:

- List sessions from disk for a game project (`game_projects/<id>/saved/`).

Success response (`200`) shape:

- `gameProjectId`
- `sessions[]` with `sessionId` and `createdAt`

#### `GET /run/:runId/state`

Purpose:

- Return DB-backed run state projection used by frontend as source of truth.

Success response (`200`) shape:

- `runId`
- `gameProjectId`
- `messages[]`
- `debugEntries[]`
- `nextTurn`

#### `POST /run/:runId/open-saved-folder`

Purpose:

- Open OS file explorer at selected session folder for quick inspection.

#### `POST /turn`

Purpose:

- Execute one engine turn for an existing run.

Request contract (current):

- `runId`: run identifier
- `turn`: turn number
- `playerInput`: freeform player text
- `playerId`: acting entity id

Request example:

```json
{
  "runId": "4de6c31b-d535-4cf6-b8f4-f248f87f5f4c",
  "turn": 1,
  "playerInput": "I order the crew to seal the hull breach.",
  "playerId": "entity.player.captain"
}
```

Success response (`200`) shape:

- `intent`: structured `ActionCandidates`
- `loremaster`: structured loremaster assessments
- `proposal`: module `ProposedDiff`
- `committed`: `CommittedDiff`
- `warnings`: non-fatal pipeline warnings
- `narrationText`: player-facing prose for this turn

#### `POST /turn/step/start`

Purpose:

- Start stepped router execution for one turn.
- Record player input and initialize paused execution state.

Success response (`200`) shape:

- `runId`
- `turn`
- `execution`: `TurnExecutionState`
- `pipelineEvents[]`: starts with frontend input acceptance event

#### `POST /turn/step/next`

Purpose:

- Execute exactly one next router stage in stepped mode.

Success response (`200`) shape:

- `runId`
- `turn`
- `execution`: updated `TurnExecutionState` (cursor/completed)
- `pipelineEvents[]`: accumulated ordered events
- `result`: final trace payload when completed, otherwise `null`

#### `GET /run/:runId/turn/:turn/pipeline`

Purpose:

- Read ordered pipeline timeline for a specific turn (in-progress or completed).

Success response (`200`) shape:

- `runId`
- `turn`
- `execution`: `TurnExecutionState | null`
- `events[]`: ordered `PipelineStepEvent`

Validation error (`400`) example:

```json
{
  "error": "Missing required turn payload fields."
}
```

## 7) Game project system

Game projects are self-contained content packages in `game_projects/<id>/`:

- `manifest.json`
  - project metadata
  - entry state
  - module bindings
- `lore/`
  - narrative source material
- `tables/`
  - structured world data
- optional `modules/`
  - game-specific code plugins (future)

This model supports shipping multiple distinct games on the same engine runtime.

## 8) LLM provider strategy

Provider abstraction is designed to allow transparent model backend swapping:

- online APIs during early development
- local models later (for example via Ollama and `qwen2.5:7b-instruct`)

Model/provider routing is designed to be per-module and configurable in game project manifests (for example cheaper model for intent extraction, stronger model for prose).

Recommended local default for early development:

- Ollama
- `qwen2.5:7b-instruct`

Each standalone module service currently has its own provider selection from env and can evolve to shared provider utilities.

## 9) Frontend architecture notes

Current UI behavior:

- stacked Game UI (top) and Debug UI (bottom)
- session selector backed by `GET /game_projects/:id/sessions`
- turn state source of truth from `GET /run/:runId/state`
- debug turn navigation with structured trace cards
- debug timeline rendering from ordered `pipelineEvents`
- step mode controls: start, next stage, run-to-end without re-execution

Future enhancements:

- richer trace visualization (collapsible conversations, copy helpers)
- optional generic widgets for inventory/crew/economy views

## 10) Non-goals (current phase)

- No multiplayer support.
- No strict deterministic replay requirement.
- No custom mod-defined UI system in MVP.

## 11) Evolution roadmap

Near-term:

- Harden module service contracts and router policy enforcement.
- Expand deterministic invariants and policy checks.
- Add automated tests for schema/misuse/turn behavior.
- Add OpenAPI and generated API docs.

Medium-term:

- Add authoritative simulation modules (stealth, damage, economy, crew).
- Add retrieval and contradiction checks.
- Expand game project plugin loading and safety controls.

## 12) LLM module responsibilities (detailed)

The core rule across all modules is:

- **LLMs propose; the engine commits.**
- Prose is never source-of-truth state.

### 12.1 Shared principles for all LLM modules

#### Structured I/O (IR-first)

- Every non-prose LLM module returns schema-valid JSON IR, not freeform text.
- Freeform output is allowed only for the final `proser` module.
- Benefits:
  - composable modules
  - deterministic validation
  - bounded schema retries
  - debuggable turn diffs

#### Untrusted input boundary

- Player text and retrieved lore are treated as untrusted content.
- Prompts and parsers must prevent prompt injection from bypassing engine rules.
- No module should treat user text as policy override instructions.

#### Bounded retries and fallback

- On schema/invariant failure:
  1. retry with constrained "fix JSON only" instruction
  2. cap retries to a fixed maximum `N`
  3. if still invalid, return safe fallback behavior
- Typical fallback:
  - ask a clarification
  - emit minimal safe response
  - avoid mutating critical state

### 12.2 Intent Extractor (`intent_extractor`)

Responsibility:

- Convert raw player text into one or more structured action candidates.
- Decide what the player is trying to do, not what happens.

Inputs (turn context subset):

- `player_text`
- `recent_turn_summary`
- `known_entities_in_scope` (IDs/names/tags)
- `current_anchor`
- optional `disambiguation_policy`

Output (`ActionCandidates`):

- `candidates[]` with:
  - `intent` (for example `attack`, `talk`, `move`, `inspect`, `use_item`)
  - `params` (target/tool/destination/topic)
  - `confidence` (`0..1`)
  - optional `consequenceTags[]`
  - optional `clarificationQuestion`
- `rawInput`

Notes:

- Keep intent taxonomy small and extensible.
- Clarification can be triggered here or deferred to arbiter.
- If clarification is required, runtime should return a proser clarification response and commit minimal or no world changes for that turn.

### 12.3 Lore Retriever (`lore_retriever`)

Responsibility:

- Retrieve the minimal lore and state context relevant to this turn.

Inputs:

- `ActionCandidates`
- current facts/anchors
- game project lore corpus

Outputs:

- `retrieved_lore[]` with source IDs
- `retrieved_facts[]` relevant to constraints and resolution

### 12.4 LoreMaster / Plausibility Critic (`loremaster`)

Responsibility:

- Classify candidate actions as plausible, constrained, or clarification-needed.
- Attach consequence tags and recommended resolution mode.

Inputs:

- `ActionCandidates`
- `retrieved_lore`
- `relevant_facts`
- optional tone/style constraints

Output (`LoremasterOutput` payload):

- per candidate:
  - `candidateIndex`
  - `status`: `allowed | allowed_with_consequences | needs_clarification`
  - `consequenceTags[]`
  - optional `clarificationQuestion`
  - `rationale`

Critical constraint:

- This module does not emit committed state diffs.
- It emits constraints and consequence guidance only.

### 12.5 Default Simulator (`default_simulator`)

Responsibility:

- When no authoritative deterministic simulator exists, propose believable state changes and observations.

Inputs:

- selected action candidate
- LoreMaster constraints/tags
- relevant `WorldState` slice
- relevant `ViewState(player)` slice
- retrieved lore
- simulation budget/conservatism settings

Outputs (`ProposedDiff` plus narrative grounding payload):

- scoped `diffs[]` writes (`world` or `view:player`)
- entity/fact updates with stability suggestions
- optional anchor creation
- `outcome_facts[]` for narration grounding
- optional `new_entities[]`
- optional `observation_events[]`
- optional `randomness_used` metadata

Notes:

- This module is soft and story-friendly.
- LoreMaster tags should directly steer partial-failure realism.

### 12.6 Arbiter / Judge (`arbiter`)

Responsibility (final LLM decision stage):

- Select intent interpretation.
- Merge/resolve competing resolver proposals.
- Set/adjust fact stability (`ephemeral/session/canon`).
- Enforce authoritative module override policy.
- Provide narration constraints.

Authority boundary:

- For **authoritative resolver outputs**, arbiter is restricted to:
  - accept
  - reject due to contract/invariant/input-version errors
  - request bounded rerun/correction only when the authoritative resolver itself is non-deterministic (for example LLM-based)
  - attach metadata/rationale
- Arbiter should not rewrite authoritative domain mechanics directly.
- For deterministic/classical authoritative modules, same input should produce same output; "rerun for a different answer" is not a valid operation.
- For **advisory resolver outputs**, arbiter may freely edit/merge/replace proposals before commit.

Inputs:

- player input
- `ActionCandidates`
- plausibility/constraint report
- all resolver `ProposedDiffs[]`
- state summaries
- invariant checklist
- optional authoritative module policy

Output (`CommittedDiff`):

- final committed `diffs[]`
- `selected_action`
- short `rationale`
- `narration_directives`:
  - `must_include`
  - `must_not_claim`
- optional `retry_request`

Post-arbiter checks (current):

- schema checks happen at module-output boundaries
- additional invariant/policy checks are roadmap items (not a standalone validator module right now)

### 12.7 Critic / Verifier (`critic` / `verifier`)

Responsibility:

- Detect contradictions and unsafe commits before presentation.

Inputs:

- candidate committed diff
- invariant set
- state summary

Outputs:

- `ok: true | false`
- `issues[]` when false
- optional fix/rerun recommendation

Implementation path:

- Start deterministic.
- Add optional LLM-based contradiction critique later.

### 12.8 Proser / Narrator (`proser`)

Responsibility:

- Generate player-facing narrative text grounded only in committed state and allowed view.

Inputs:

- `ViewState(player)` summary (or world summary if no hidden-info mode)
- `CommittedDiff` and `outcome_facts`
- `narration_directives` (`must_include` / `must_not_claim`)
- style settings (voice/tense/length)

Outputs:

- `narration_text`
- optional structured debug extras (observations/recap)

Critical rule:

- Proser must not invent new state facts.
- If uncertain, remain vague or request upstream clarification flow.

### 12.9 One-turn interaction summary

1. player input arrives
2. intent extractor proposes candidates
3. loremaster retrieves lore and scores plausibility/consequences (pre-check)
4. simulators run (authoritative modules first, default simulator fallback)
5. loremaster post-check returns narration constraints
6. arbiter module selects commit candidate (currently always accepts validated proposal)
7. proser narrates committed outcome only
8. world state update persists trace, committed diff, and snapshot

All module inputs/outputs and final commit are persisted in the event log for audit/debug.

Clarification path:

- If intent/loremaster marks `needs_clarification`, the engine returns a clarification question and commits minimal/no world mutation until clarified input arrives.
- Concrete behavior example: if current context has no valid enemies in scope and the player enters `attack`, the engine should ask a disambiguation question such as `What do you want to attack?` and avoid speculative combat state mutations on that turn.

### 12.10 Practical MVP defaults

For fastest delivery:

- implement `intent_extractor`, `loremaster`, `default_simulator`, `arbiter`, `proser`
- keep bounded retries for structured module outputs
- defer critic/verifier LLM pass initially
- use one model backend first (for example `qwen2.5:7b-instruct`), specialize per module later

## 13) Architecture delta: near-term implementation roadmap

This section translates current design direction into concrete implementation phases.

### 13.1 Phase A (stabilize core contracts and runtime)

Goals:

- Formalize a turn-scoped blackboard payload as a first-class runtime object.
- Keep module communication IR-first and schema-validated.
- Make structured decoding behavior measurable in production logs.

Required outputs:

- `TurnBlackboard` contract in `packages/shared`:
  - `context`
  - `candidates`
  - `constraints`
  - `proposals`
  - `committed`
  - `citations`
  - `warnings`
- Backend instrumentation per module:
  - parse success/fail
  - retry count
  - timeout count
  - fallback count
  - latency/token usage

Acceptance criteria:

- Every module read/write is represented in persisted `module_trace` events in structured form.
- `POST /turn` success payload remains stable while adding non-breaking blackboard fields.

### 13.2 Phase B (memory hierarchy + retrieval provenance)

Goals:

- Introduce memory tiers inspired by virtual context management:
  - `working`
  - `episodic`
  - `semantic`
- Implement `lore_retriever` with provenance-first outputs.

Required outputs:

- Retrieval contract in `packages/shared`:
  - `retrievalQuery`
  - `evidenceItems[]` (`id`, `source`, `text`, `score`)
  - `selectedEvidenceIds[]`
- Session persistence integration:
  - persisted retrieval trace in `module_trace` events
  - citation references available to `proser`

Acceptance criteria:

- Any factual claim used by simulator/proser can be traced to source evidence in debug output.
- Retrieval can be toggled per game project manifest without code changes.

### 13.3 Phase C (visibility/fog-of-war + deterministic authority)

Goals:

- Enforce player-view constraints at simulation and narration boundaries.
- Strengthen deterministic authoritative module policy.

Required outputs:

- Visibility-aware state slices:
  - `WorldState` (truth)
  - `ViewState(player)` (known)
- Deterministic gate checks:
  - no “retry for different outcome” for deterministic authoritative modules
  - explicit reject reasons (`contract_error`, `invariant_error`, `input_version_error`)

Acceptance criteria:

- Hidden information never appears in `narrationText` unless surfaced by observation events.
- Deterministic module outputs are either accepted or rejected with typed reason; not creatively rewritten.

### 13.4 Phase D (ReAct-style debug traces and operator control)

Goals:

- Improve observability of LLM decisions by normalizing traces to:
  - `thought`
  - `action`
  - `observation`
- Support safe human intervention hooks for debugging.

Required outputs:

- Trace schema extension for module conversations with optional ReAct segmentation.
- UI improvements:
  - grouped traces by module
  - copyable structured blocks
  - explicit retry/fallback badges

Acceptance criteria:

- Debug UI makes each module’s decision path inspectable without reading raw prompt dumps.
- Manual diagnostics can identify whether failure came from retrieval, reasoning, schema, or policy gate.

### 13.5 Out-of-scope for this roadmap

- Multiplayer runtime model.
- Full deterministic replay guarantee.
- Graphical map/continuous geometry as required default.
