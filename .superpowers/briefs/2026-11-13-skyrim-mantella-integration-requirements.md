# Morpheus Engine: requirements for the Skyrim / Mantella integration

From: the Claude session that runs the user's Skyrim setup (`~/Skyrim`). To: the agent working on MorpheusEngine.
Date: 2026-11-13. Morpheus commit analysed: `c1cedaf`. Mantella commit analysed: `c0c1ae6` (v0.14 + 8).

This turns your proposal (steps 1-6: `morpheus_server`, per-call grammar, world authority, per-speaker memory, prompt
pack, non-player turns) into concrete requirements, contracts and acceptance tests. Where I deviate from your proposal,
the reason is given. Background maps, if you want them: `~/Skyrim/notes/mantella-architecture-2026-11-13.md` (Mantella
server, protocol, file:line references) and `~/Skyrim/notes/morpheus-reuse-analysis-2026-11-13.md` (Morpheus coupling,
file:line references).

---

## 1. Goal and constraints

**Goal.** Skyrim NPCs talk through Mantella's game side (Papyrus + SKSE_HTTP, unchanged), with a forked Mantella server
in front. The fork hands the "mind" to Morpheus: grammar-constrained action choice validated by the engine, exact token
budgets with a cache-friendly prompt, per-speaker memory with retrieval and folding. Skyrim owns the world and its
mechanics; Morpheus never simulates them.

**Constraints that shape every requirement below.**
- **The user keeps developing Morpheus for its own game.** We consume Morpheus as a pinned upstream commit and update by
  moving the pin. So: Skyrim-specific code stays on our side; Morpheus exposes generic, versioned seams; no requirement
  here may degrade Morpheus's own game.
- **Process boundary = HTTP on localhost.** Mantella is Python (AGPL-3.0) and already speaks an OpenAI-compatible API.
  No Python bindings. Morpheus stays MPL-2.0 in its own process.
- **Linux, natively.** Builds in an Ubuntu 24.04 distrobox with clang/gcc + CMake + Ninja (no Visual Studio). The GPU is
  NVIDIA; llama.cpp there is built with **CUDA** 13.4 for sm_86.
- **Hardware.** RTX 3080 10 GB runs the game. An **RTX 3060 Ti 8 GB** (ordered) will be dedicated to the LLM. Model:
  **Qwen3.5 9B HauhauCS Q4_K_M** (5.2 GB, dense), 16k context. Fallback until the card is in: the same model partly
  offloaded on the 3080 next to the game (about 2 GB VRAM free there).
- **Concurrency is real.** In one Mantella conversation there are several model calls in flight or in quick alternation:
  the action choice, the spoken line (streamed), summaries, and in group conversations several NPCs.

---

## 2. Architecture

```
Skyrim (Proton) ──HTTP :4999──> Mantella fork (Python)         ──HTTP :4998──> morpheus_server (C++, Morpheus repo)
 Papyrus + SKSE_HTTP            protocol with the game,                         sessions, memory, grammar, prompts
 (unchanged)                    TTS/STT, voice files                            │
                                                                                └─> model access (see R2)
```

Ownership:
| Concern | Owner |
|---|---|
| Game protocol, TTS/STT, voice-file hand-off, Papyrus action dispatch | Mantella fork (ours) |
| Observation of the world (location, time, actors, events) | Skyrim, via Mantella's request fields |
| Mechanics: whether an action succeeds | Skyrim (reports back as events) |
| Prompt assembly, token budgets, cache-friendly ordering | Morpheus |
| Action proposal: grammar per call + validation | Morpheus |
| Memory: per-speaker records, retrieval, folding, condensing | Morpheus |
| Skyrim-specific text (prompt pack, action catalog, observation schema) | Mantella fork, sent to Morpheus as data |

---

## 3. What we do WITHOUT Morpheus first (so you don't build it)

Your steps 1-2 (exact counts, per-call grammar, cache-friendly prompt, action schema with present NPCs) need no Morpheus
change: llama.cpp's own `llama-server` already serves the OpenAI chat API with a per-request `json_schema`/`grammar`,
`/tokenize`, prompt-cache reuse and parallel slots, and we run it today. We will do those in the Mantella fork against
llama-server and play with them. **Please don't build `morpheus_server` as a plain OpenAI proxy for that purpose.** It
becomes worth building when it carries Morpheus logic (sessions, memory, world authority): requirements R4-R9 below.

---

## 4. Requirements

Priority: **P1** = needed for the first Morpheus-backed version; **P2** = next; **P3** = later / nice to have.
Sizes: S/M/L as rough effort. Acceptance criteria are what we'll test against.

### R1 (P1, S) Consumable build
- `MORPHEUS_BUILD_APP` and `MORPHEUS_BUILD_TESTS` default to `PROJECT_IS_TOP_LEVEL`; new `MORPHEUS_BUILD_SERVER`
  (default ON when top level) for `morpheus_server`.
- No target-name collisions when vendored: guard or namespace `sqlite3` and `nlohmann_json` (`if(NOT TARGET ...)` or
  `FIND_PACKAGE_ARGS`); ALIAS targets `Morpheus::core`, `Morpheus::llama`, ...
- A Linux/Ninja `CMakePresets.json` preset next to the Visual Studio ones.
- llama.cpp URL updated to `ggml-org` (CMakeLists.txt:84).
- **Accept:** `cmake --preset linux-release && cmake --build` on Ubuntu 24.04 builds `morpheus_server` and the tests;
  `FetchContent` of Morpheus into another CMake project builds only the libraries.

### R2 (P1, M) Model access suitable for a shared, multi-slot GPU host
`morpheus_server` must reach the model in a way that supports concurrent calls without evicting each other's cached
prefix. Two acceptable implementations; **we prefer (a)**:

**(a) HTTP backend to llama-server** (your R4 of my analysis; matches the architecture spec's planned "cloud backend").
- Split `LlmBackend` (src/inference/Llm.h:82-113) into `Tokenizer` (CountTokens, CountPromptTokens, ContextSize),
  `ChatModel` (Complete, Save/RestoreState) and `Embedder` (Embed, EmbeddingModelId); keep `LlmBackend` as their union
  for existing callers. Move `SamplingFrom` out of `inference` (Llm.h:13, 48-49) so `inference` has no `core` dependency.
- `OpenAiCompatBackend`: `/tokenize` (exact counts), `/apply-template` (exact templated counts), `/completion` or
  `/v1/chat/completions` with `grammar` / `json_schema`, `id_slot` + `cache_prompt` for slot affinity, `/embedding`
  (a second llama-server instance may serve the embedder). HTTP client behind a CMake option.
- Why preferred: one model host for every consumer, its slots/KV/CUDA/device pinning already work and are tuned
  (`~/Skyrim/scripts/llm.sh`); much less engine work than (b).

**(b) In-process, if you'd rather keep inference inside Morpheus.** Then all of these are required, not optional:
- CMake GPU choice `MORPHEUS_GPU_BACKEND=Vulkan|CUDA|CPU` (stop FORCE-ing `GGML_VULKAN`, CMakeLists.txt:79).
- `LlamaConfig` (src/llama/LlamaCppBackend.h:32-40): `nThreads`, `nThreadsBatch`, `devices`/`mainGpu`/`splitMode`,
  `flashAttention`, KV cache types (q8_0), `nBatch`/`nUbatch`, `useMmap`, `tensorBufferOverrides`, separate
  `embeddingGpuLayers`, **optional** embedding model.
- **Multiple KV sequences**: `n_seq_max = N`, one cached token list per slot, `Complete(..., slot)`, LRU eviction, per-slot
  save/restore (cpp:93-104, 160-166, 284-298, 519-567). Today `n_seq_max = 1`, so alternating action/speech/summary
  calls and several NPCs would evict the shared prefix on every switch.
- A server-side request queue so concurrent HTTP requests map onto slots safely (the backend is single-owner-thread).

- **Accept (either way):** with 3 concurrent sessions alternating action and speech calls on a 16k context, the second
  and later calls of a session report prompt-cache reuse > 80 % of their prompt tokens (`Completion` telemetry).

### R3 (P1, M) Schema -> grammar -> validator library (`morpheus_grammar`)
Your step 2 needs the grammar per call; we need it as a reusable library because the action list changes every exchange.
- Public builder from the private helpers in `ResolveGrammar.cpp:16-66` (bounded string, int range, literal,
  alternatives, bounded list).
- `Grammar FromSchema(const json& schema)` over a JSON-Schema subset: `object` with ordered required keys,
  `enum`/`const`, `integer` with min/max, `string` with maxLength, `array` with maxItems, `oneOf` with a discriminator
  key. Bounds come from the schema (not constexpr).
- `ValidationResult Validate(const json& value, const json& schema)`: re-checks exactly what the grammar allows and
  returns model mistakes as values (field path + message), never exceptions (like `ParseReviewOutput`, Review.cpp:20).
- Re-express `BuildResolveGrammar` / `BuildLookupGrammar` / `BuildReviewGrammar` on it; their tests stay green.
- **Accept:** for the example action schema in §5.2, every grammar parses in llama.cpp (extend
  tests/llama/LlamaGrammarTests.cpp), 1000 random grammar-constrained samples all validate, and hand-made invalid
  values (unknown action, absent NPC name, missing required argument, over-long string) each fail with the right path.

### R4 (P1, M) `morpheus_server`: session API
An outward-facing tool (your step 1, but carrying Morpheus logic). Single process, localhost only, JSON over HTTP.
Requests are idempotent where marked; every response carries `api_version`.

| Endpoint | Purpose |
|---|---|
| `GET /health` | ready + model ids + `api_version` |
| `POST /v1/runs/{run_id}/open` | open or create a run (= a Skyrim save line: Mantella's `world_id`); body: prompt pack (R7), action catalog (§5.2), observation schema (§5.1). Idempotent. |
| `POST /v1/runs/{run_id}/conversations` | start a conversation: participants (§5.1 actors) + observation; returns `conversation_id` |
| `POST /v1/conversations/{id}/turn` | one exchange: who speaks (NPC id, or "decide"), new player text or none, new events, observation delta. Returns the action proposal (validated, §5.3) as soon as it's decided, then streams the spoken line (SSE: `action`, `token`, `sentence`, `done` events) |
| `POST /v1/conversations/{id}/events` | record events without a turn (game results, ambient lines, an NPC leaving) |
| `POST /v1/conversations/{id}/participants` | join / leave |
| `POST /v1/conversations/{id}/end` | close: write per-speaker records, trigger folding (async) |
| `POST /v1/tokenize`, `POST /v1/embeddings` | utilities (exact counts for the fork's own budgeting) |

- Two calls per spoken turn, as you proposed: **resolve** = action JSON (short, grammar-constrained), **narrate** = the
  spoken line, streamed, prompt extending resolve's so it's nearly free. Speech never goes inside the JSON (Mantella
  voices it sentence by sentence).
- `sentence` events must be emitted as soon as a sentence is complete, so TTS can start on the first one.
- **Accept:** a scripted conversation (fixtures we'll provide, §6) runs end to end with the fake backend
  (tests/fakes) and with a real model; first `sentence` event < 1.5 s after the request on the 3060 Ti with warm cache.

### R5 (P2, L) World authority: `HostedWorld` (your step 3)
- `Session::PlayTurn` calls through an interface with your three operations: `Observe()`, `ProposalGrammar()`,
  `Adjudicate(proposal)`. `OwnedWorld` = today's behaviour (schema, dice, `ApplyProposal`, snapshots).
- `HostedWorld`:
  - the observation arrives with each request and is validated against a **host-supplied observation schema**
    (§5.1); a malformed request is an error naming the field;
  - `ProposalGrammar()` = R3 grammar from the action catalog filtered by the current observation (allowed actions,
    present actor ids as enums);
  - `Adjudicate()` accepts a valid proposal as **requested**, not applied; the outcome arrives later as an event
    (`action_result` with `succeeded`); a refused action is recorded like a rejected change;
  - no dice, no checks, no snapshots in the grammar or the turn.
- **Accept:** Morpheus's own game passes its existing tests unchanged through `OwnedWorld`; a HostedWorld test drives
  turns purely from fixture observations and events.

### R6 (P2, L) Per-speaker memory (your step 4)
- Turn records carry `speaker` and `listeners` (actor ids present). Summaries and notes are keyed by actor id.
- Retrieval for actor X only considers records X witnessed (speaker or listener) + lore + memories shared to X.
- Shared memory: an explicit operation copies a summary from A to B with provenance ("told by A").
- Folding/condensing per actor, asynchronous after `end`, budgeted in exact tokens (your existing fold/condense).
- A run = one save line (`world_id`). Store schema version bump with a migration for existing Morpheus runs.
- Reloading an older save: an endpoint to roll a run back to a game-time stamp (records after it are hidden, not
  deleted), because players reload.
- **Accept:** in a 3-actor fixture where actor C leaves halfway, C's retrieval never returns lines said after it left;
  B's memory of A's secret appears only after a share; rollback hides later records and restores them on roll-forward.

### R7 (P2, M) Host-supplied prompt pack (your step 5)
- Everything that is text today (`turn/EngineText.h`, the narrate instruction, resolve/review/fold templates) can be
  supplied by the host per run as data, with Morpheus's current texts as the default pack.
- Layer order fixed by the engine for cache reuse: stable pack text -> stable world lore -> participant bios in join
  order -> memory/retrieval -> volatile observation (time, weather, events) -> conversation tail. A join invalidates
  the cache only from the bios onward.
- Template variables are named and validated at load (unknown variable = error at `open`, not at turn time).
- **Accept:** byte-identical prompt prefix across turns of one conversation up to the volatile layer (test by comparing
  rendered prompts); pack validation errors name the template and variable.

### R8 (P2, M) Turns not started by the player (your step 6)
- `turn` accepts `speaker: <npc id>` (NPC to NPC, radiant conversations) or `speaker: "decide"` (the engine picks the
  next speaker among participants, constrained by grammar).
- Events can be recorded without a turn (R4 `/events`), and are included in the next turn's volatile layer.
- **Accept:** a fixture with two NPCs talking for 6 turns with no player input produces alternating, valid turns.

### R9 (P3, S) Smaller items
- Sampling fields: min_p, repeat/presence penalty or DRY, stop strings; an explicit `callTag` in `GenerationSpec` so
  `CallKindOf` (Replay.h:27) no longer parses grammar text.
- Thinking formats as data (`ThinkingFormat {open, close, closeNudge}`), presets for Qwen and "none"; thinking OFF by
  default for dialogue (latency).
- Review/renarrate (your Review pass) available per turn as an option, off by default: Mantella streams speech, so a
  review means buffering the whole line (~1-2 s); we want to measure it before enabling it.

---

## 5. Data contracts (Skyrim side, sent as data)

These are what the Mantella fork will send. Names are suggestions; please keep them stable once implemented.

### 5.1 Observation and actors
Derived from Mantella's `mantella_context` / `mantella_actors` (Mantella-Spell `MantellaConversation.psc:983-1060`).
```json
{
  "game_time": {"days_passed": 213.42, "hour": 14},
  "location": "Whiterun, The Bannered Mare",
  "weather": "clear",
  "actors": [
    {"id": "0001A6A4", "name": "Hulda", "is_player": false, "gender": "female", "race": "Nord",
     "relationship_rank": 1, "in_combat": false, "hostile": false, "voice_type": "FemaleEvenToned",
     "equipment": {"body": "Barkeeper's Clothes"}, "bio": "..."},
    {"id": "00000007", "name": "Aela", "is_player": true, "description": "..."}
  ],
  "nearby": [{"id": "0001B07C", "name": "Saadia", "distance_m": 6.5}],
  "events": ["Aela picked up an Iron Sword.", "Hulda started following Aela."]
}
```
The host also sends the JSON Schema of this object at `open` (HostedWorld validates against it).

### 5.2 Action catalog
Derived from Mantella's `data/actions/*.json` (identifiers, parameter schemas, contexts they're offered in). Per turn the
host marks which are allowed now. The grammar must restrict actor-id parameters to the actors actually present.
```json
{
  "actions": [
    {"id": "none"},
    {"id": "follow", "params": {}},
    {"id": "wait", "params": {}},
    {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
    {"id": "give_item", "params": {"item": {"type": "string", "maxLength": 48}}},
    {"id": "end_conversation"}
  ]
}
```
Resulting grammar shape (R3): `oneOf` discriminated by `"action"`, `"target"` an enum of present actor ids. No extra keys
allowed (this is what fixes our measured Qwen failure: an `"intent"` key added to `wait`).

### 5.3 Turn result
```json
{"speaker": "0001A6A4", "action": {"id": "follow", "params": {}}, "line": "Lead the way, then."}
```
`action` arrives first (SSE `action` event), `line` streams after it (`token`, `sentence`). The host turns the action into
Mantella's `{"identifier": ..., "arguments": ...}` itself.

### 5.4 Events
`{"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true, "text": "Hulda is now following Aela."}`
`{"type": "ambient", "text": "..."}`, `{"type": "left", "actor": "..."}`, `{"type": "joined", "actor": "..."}`.

---

## 6. Updating over time (how we stay in step with your work)

- **API versioning:** `api_version` (semver) on every response; breaking changes bump the major version and are listed in a
  `CHANGELOG` entry with a migration note. We pin a Morpheus commit and move the pin deliberately.
- **Contract fixtures in the Morpheus repo:** we'll provide `tests/hosted/fixtures/*.json` (open, conversations, turns,
  events, expected validations) built from real Mantella traffic. Please run them in CI with the fake backend, so a change
  that would break the Skyrim host shows up in Morpheus's own tests.
- **No Skyrim knowledge in the engine:** prompt pack, action catalog and observation schema are data from the host.
  If something can only be done with Skyrim knowledge inside Morpheus, flag it instead of adding it.

## 7. Non-goals

- Python bindings or embedding Morpheus in Mantella's process.
- Dice, checks or any game mechanics for Skyrim.
- TTS, STT, voice files, the game protocol: all stay in the Mantella fork.
- A plain OpenAI proxy (§3).

## 8. Open questions for you

1. R2: (a) HTTP backend or (b) in-process with multi-slot? If (b), how do you want to share one GPU model between
   Morpheus's own game and the Skyrim host on the same machine?
2. R5: does `HostedWorld` live in the Morpheus repo (generic, schema-driven) or should the interface be public so we
   implement it on our side? We'd prefer it in Morpheus, generic, so your tests cover it.
3. R6: is per-actor keying a change to `RunStore`'s schema you're comfortable making, or would you rather a separate store
   type for hosted runs?
4. Anything above that conflicts with Morpheus's own design direction: tell us and we'll adapt the host side.
