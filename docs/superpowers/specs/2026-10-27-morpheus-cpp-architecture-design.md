# MorpheusEngine (C++) - Target Architecture

Date: 2026-10-27. Status: approved design, pre-implementation.

## Goal and constraints

A text-based RPG engine driven by a local LLM, built first for the author to play their own worlds.

- **Local-first.** llama.cpp runs inside the engine process. A cloud provider can be added behind the same interface later.
- **Runs on the author's laptop.** The machine is an RTX 3070 Laptop GPU (8 GB VRAM) with 32 GB RAM. That means quantized GGUF chat models of roughly 4-12B parameters with 16k+ context, some layers offloaded to the CPU where needed. The chat model should be multimodal, so it can also read images (see the visualization spec). A larger GPU only removes model swapping and allows bigger models; nothing is redesigned.
- **The engine owns the game.** It holds structured state and rolls the dice. The LLM proposes outcomes and narrates them.
- **C++20, one process, one executable.** No HTTP between modules, no router, no Ollama.

## Process and modules

Two threads talk only through two thread-safe queues:

- **UI thread:** the ImGui render loop. It never blocks.
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time.
- **Commands (UI to worker):** `StartRun`, `LoadRun`, `SubmitInput`, `Undo`, `Regenerate`, `Cancel`.
- **Events (worker to UI):** `TokenStreamed`, `TurnResolved`, `TurnCompleted`, `StateChanged`, `Error`.

Each module is a static library. Dependencies flow downward only.

| Module | Responsibility | Depends on |
|---|---|---|
| `core` | World definition, schema-typed state, change validation, dice, turn record types. Pure logic; its only I/O is reading world folders. | - |
| `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings | core |
| `inference` | `LlmBackend` interface; `LlamaCppBackend` (tokenize, KV sequence reuse, grammar sampling, embeddings) | - |
| `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
| `turn` | Turn orchestration and between-turn maintenance (embedding, folding) | all above |
| `app` | SDL3 + Dear ImGui frontend: play panel and inspectors | turn (plus read-only views) |

**Build:** CMake, with dependencies pulled through FetchContent and pinned to commit SHAs: llama.cpp, nlohmann/json, SDL3, the ImGui fork, the SQLite amalgamation, Catch2 v3. Warnings are treated as errors (`/W4 /permissive-`). Debug builds use AddressSanitizer.

## Turn flow (resolve, then narrate)

1. **Retrieve (engine).** Embed the player input. Rank lore entries and past turns by cosine similarity, boosting entities named in the input. Trim the result to the retrieval budget.
2. **Resolve (LLM, JSON grammar).** The model returns `{intent, checks[], changes[]}`.
   - Each check has `stat`, `difficulty`, `stakes`, `on_success[]` and `on_failure[]`.
   - Each change is an op: `set`, `add`, `remove` or `create` applied to a `path`, with an optional `value` and a `reason`.
   - The grammar is generated from the world's state schema, so only valid paths and enum values can be emitted.
3. **Apply (engine).** Roll each check with the run's seeded RNG and select its outcome branch. Validate each change against the schema (path exists, type, range, `create` only for creatable entity types). Apply the valid changes and record the rejected ones with a reason.
4. **Narrate (LLM, streamed prose).** The prompt is the resolve prompt with three things appended: the resolve output, the engine's resolution record (rolls, outcomes, applied and rejected changes), and the narrate instruction. The model narrates the outcome. It does not decide it.
5. **Commit (engine).** Write the turn row and the state snapshot in one transaction.

**Between turns, while the worker is idle:**
- Embed the finished turn.
- If the recent-turns layer is over budget, fold its oldest half into the story summary with one summarization call.

**Undo and regenerate:** delete the rows after turn N-1, reload that turn's snapshot, and re-run.

**`memory_mode` (per model/settings).** This setting decides who manages memory.
- `engine` (default): memory is fully deterministic.
- `assisted`: the resolve grammar additionally allows:
  - up to N `lookup` requests (lore or history) before resolving, with results appended to the tail;
  - append-only `notes`.

The model never edits or deletes memory.

**Error policy.** Engine bugs and model mistakes are handled differently.
- **Engine bugs** (broken invariants, corrupt data, bad world files): assert or throw, and fail loudly.
- **Model mistakes** (invalid changes, unknown entities, lookups over budget): reject, log and show in the inspector, then continue the turn. Grammars guarantee the output parses, so v1 has no retry loops.

## Context layout and KV cache

Layers run from most stable to least stable. llama.cpp reuses the longest common token prefix and evaluates only what follows it.

```
STABLE PREFIX (cached, only grows)
  L0 engine rules + output format         static
  L1 world rules, tone, always_on lore    static per world
  L2 story summary                        changes only on fold
  L3 recent turns transcript              append-only
VOLATILE TAIL (rebuilt per call)
  T1 rendered state view
  T2 retrieved lore / history (+ assisted lookups)
  T3 player input
  T4 task instruction (resolve) -> resolve output + resolution record + narrate instruction
```

- **Token counts are exact,** from the loaded model's tokenizer.
- **Budget:** `n_ctx - reserved generation (resolve cap + narrate cap) - tail allowance`. L0-L2 have fixed caps; L3 gets the remainder.
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14 -> success; location -> bridge_hatch]`). Full resolve JSON stays in the database.
- **Chat template.** The model's chat template is applied once per layer, so earlier tokens never change.
- **Cloud mapping.** The cloud backend maps the same layering onto prompt caching, with a breakpoint at the end of L3.

## Inference interface

```
LlmBackend
  count_tokens(text) -> int
  complete(LayeredPrompt, GenerationSpec{grammar?, max_tokens, sampling}, on_token, cancel)
      -> Completion{text, tokens_reused, tokens_evaluated, timings}
  embed(texts) -> vectors
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
```

- `LlamaCppBackend` loads two GGUF models in-process: a chat model and an embedding model.
- JSON schemas are converted to GBNF grammars with llama.cpp's json-schema-to-grammar.
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type (resolve runs cooler, narrate warmer), and `memory_mode`.

## World data and persistence

```
game_projects/<id>/
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode; unknown keys are errors
  rules.md            GM instructions -> L1
  lore.csv            subject,data[,always_on]  (RFC 4180)
  state_schema.json   entity types; fields: int{min,max} | enum | string | list<string> | map<item,count>; creatable flag
  initial_state.json
saves/<id>/<run>.db
```

| Table | Contents |
|---|---|
| `run_meta` | world id, world content hash, model id, RNG seed, settings |
| `turns` | n, input, resolve JSON, rolls, applied / rejected changes, narration, timings |
| `state_snapshots` | full state JSON after each turn |
| `summaries` | folded turn range and its summary text |
| `notes` | append-only model notes (`assisted` mode), committed in the same transaction as their turn |
| `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |

Rows are append-only, except that undo removes the rows after the target turn. Similarity search is brute-force cosine in memory. Each embedding stores the hash of the text it was computed from, and a changed hash means the vector is re-computed.

World files are read with CRLF normalized to LF. The world content hash covers each file's name, length and text, so a saved run can detect world edits reliably across checkouts.

## UI (ImGui docking windows)

| Window | Shows |
|---|---|
| Play | Transcript with streaming prose, input box, undo/regenerate, rolls inline |
| State | State tree, with last turn's changes highlighted |
| Context | Last prompt split by layer: tokens per layer, reused vs. evaluated, raw/formatted toggle |
| Turn log | Resolve JSON, rolls, applied and rejected changes, timings per turn |
| Memory | Summaries, notes, and last retrieval hits with scores |
| Settings | Model paths, context size, sampling, `memory_mode` |

## Testing (Catch2 v3)

- **`core` unit tests:** validation, change ops, dice, schema-to-grammar output.
- **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer.
- **Replay tests:** a `ScriptedBackend` returns recorded completions, so full turns run deterministically and the tests assert on state and database rows. A record mode turns real play sessions into fixtures.

## Carried over from the C# version

These carry over as ideas and content, not code:
- World folders as data, and the sandcrawler rules and lore
- One SQLite file per run
- Schema-constrained LLM output
- Context inspector and per-call token accounting
- Turn telemetry
- Memory tiers, reshaped into summaries, retrieval and notes
- The summarization prompt
- Fail-fast coding rules, adapted to C++

## Dropped

- Multiple processes, localhost HTTP, router/proxy, allowlists, aliases, and JSON pipeline templates
- Ollama and the log filter, `ManagedModule`, job objects
- IntentExtractor and the legacy Director
- Memory blocks the LLM can overwrite (replace/set)
- Token counting over a round-trip, and the chars/4 estimate
- WPF

## Not in v1

- A cloud backend implementation (the interface only)
- Persisting the KV cache to disk across sessions (saving it in RAM is in v1)
- Narrative visualization (separate spec: `2026-10-27-narrative-visualization-design.md`)
- Narration text-to-speech (separate spec: `2026-10-27-narration-tts-design.md`)
- The plot-consolidation agent ("PlotMaster")
- GOAP-driven NPCs
- Authoring tools
- Multiple players
