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
- **Commands (UI to worker):** `StartRun`, `LoadRun`, `SubmitInput`, `UndoTurn`, `RegenerateTurn`. Cancel is `Worker::Cancel`, not a command: it interrupts the running command, and a command cancelled before it runs closes with `TurnCancelled` without running.
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `NarrationRestarted`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window. `NarrationRestarted` carries the review's problems and says that the running turn's narration starts over: the `TokenStreamed` events after it are the new narration.
- Every command ends with exactly one closing event. `TurnResolved`, `TokenStreamed` and `NarrationRestarted` report a running turn's progress; `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep. None of them close a command.

Each module is a static library. Dependencies flow downward only.

| Module | Responsibility | Depends on |
|---|---|---|
| `core` | World definition, schema-typed state, change validation, dice, turn record types, settings. Pure logic; its only I/O is reading world folders and settings.json. | - |
| `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings | core |
| `inference` | `LlmBackend` interface and prompt types (no llama.cpp) | core |
| `llama` | `LlamaCppBackend`: tokenize, KV prefix reuse, grammar sampling, embeddings, state save/restore. The only module linking llama.cpp; built by the `dev` preset only. | inference |
| `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
| `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding, session), the worker thread, replay recording | all above |
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings | turn, llama (`main.cpp`'s backend factory) |

**Build:** CMake, with dependencies pulled through FetchContent and pinned to commit SHAs: llama.cpp, nlohmann/json, SDL3, ImGui (upstream's docking branch, v1.92.8-docking), the SQLite amalgamation, Catch2 v3. Warnings are treated as errors (`/W4 /permissive-`). AddressSanitizer is opt-in: the `asan` and `dev-asan` presets.

## Turn flow (resolve, then narrate)

1. **Retrieve (engine).** Embed the query: the player input, the current scene's values joined by `; ` (see "World data"), then the previous turn's digest, one per line and cut to 4000 bytes with the input first. Rank lore entries, folded past turns and (in `assisted` mode) notes by cosine similarity, adding 0.15 once for a lore subject named in the input or in a scene value (the hit's reasons: `name`, `scene`); the digest only enters the embedding. Assisted-mode lookups search with their own query only. Take at most 6 hits by score, then trim them to `retrieval_cap` tokens. Turns still in L3 are not searchable, since they are already in the prompt. In `assisted` mode the model's lookups add hits only within what is left of `retrieval_cap`, and the turn's telemetry records which of them reached the resolve prompt.
2. **Resolve (LLM, JSON grammar).** The model returns `{intent, checks[], changes[]}`.
   - Each check has `stat` (one of world.json `stats`), `difficulty`, `stakes`, `on_success[]` and `on_failure[]`.
   - Each change is an op: `set`, `add`, `remove` or `create` applied to a `path`, with an optional `value` and a `reason`.
   - The engine generates the GBNF grammar itself (`turn::BuildResolveGrammar`) from the live state and the schema, so only valid paths, value types and enum values can be emitted. Counter items already held are matched by name; a new item follows a generic name rule. The grammar also bounds sizes (checks, changes, text lengths), which bounds the resolution record.
   - Output the engine cannot read (cut off at `resolve_max_tokens`, or JSON of the wrong shape) is a model mistake: the turn goes on with nothing proposed, and the narrate call still runs.
3. **Apply (engine).** Roll each check with the run's seeded RNG and select its outcome branch. Validate each change against the schema (path exists, type, range, `create` only for creatable entity types). Apply the valid changes and record the rejected ones with a reason.
4. **Narrate (LLM, streamed prose).** The prompt is the resolve prompt with two messages appended: the resolve output (assistant), then the engine's resolution record (rolls, outcomes, applied and rejected changes) followed by the narrate instruction (user). The model narrates the outcome. It does not decide it.
   - The narrate message is fitted to `narrate_allowance`. While it is over, detail is shed in this order: the rejected changes, then applied changes from the end, then each check's stakes, then the intent is cut short. If it still does not fit, `narrate_allowance` is too small (a `DataError`).
5. **Review (LLM, JSON grammar), when `narration_check` or `compact_transcript` is on.** A standalone call, not an extension of the narrate prompt: the review rules as the system message, then the player input, the outcome rendered as the narrate message renders it, and the narration. It is about 300 to 600 tokens whatever the run's length, so it needs no reservation in the context fit. Its grammar (`turn::BuildReviewGrammar`) allows `consistent` and up to 3 `problems` of up to 200 characters (the check) and a `digest` of up to 300 characters (the compact transcript). `ParseReviewOutput` enforces the same bounds, counted in characters (code points): output that cannot be read (cut off at `review_max_tokens`, more than 3 problems, a problem over 200 characters or empty once trimmed, a digest over 300 characters) is a model mistake, and the turn goes on as if the review had found nothing. On Qwen3-4B-Instruct the review prompt replaces the narrate prompt in the backend's cache, so the next resolve call evaluates its whole prompt; Qwen3.5 and later reuse no prefix anyway.
6. **Renarrate (LLM, streamed prose), at most once, with the check on.** When the first review says the narration contradicts the outcome (a failed check narrated as a success or the reverse, an applied change left out or reversed, a rejected change narrated as if it happened), the worker sends `NarrationRestarted` and the narration is written again from the same narrate prompt, whose narrate message gains the review's problems before the narrate instruction. Every narrate message, an unreadable proposal's included, is measured against `narrate_allowance`; when it is over, detail is shed step by step and the renarrate's problem lines go last (the correction sentence stays), before a `DataError` for an allowance that is too small. The second narration is reviewed, and it and that review's digest are committed whatever the verdict: there are no loops.
7. **Commit (engine).** Write the turn row (with the last review's digest), the state snapshot and any `assisted`-mode notes in one transaction.

**Between turns (maintenance):**
- If the recent-turns layer is over budget, fold its oldest half into the story summary with one summarization call. If the summary is over `summary_cap`, condense the summaries into one. A `summary_cap` too small for even a condensed summary is a `DataError`.
- Each memory call must fit the context: its prompt plus its `max_tokens`, measured like any call. A fold takes only as many of the oldest half's turns as fit. The transcript can be far larger than the context, for a run loaded with a smaller `context_size` or turns put back by undoing past a condensed summary.
- If the model writes an empty summary, or not even one turn (or, for a condense, the summaries) fits a call, the engine writes the summary itself: a line per folded turn (its input and mechanics line), or per condensed summary, cut to the summary's size. Folding always makes progress.
- Embed the folded turns and the new notes, so retrieval can find them.
- The worker runs maintenance while no command waits, and a new command preempts it. Before a `SubmitInput` or `RegenerateTurn`, which retrieve, a due maintenance runs to completion first, so a turn always sees the previous turn's notes.

**Undo and regenerate:** delete the rows after turn N-1, reload that turn's snapshot, and re-run. Both resync the memory index with the store, so an undone turn, its notes and any summary covering it leave retrieval too.
- Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number). `SamplingSeed` also takes the call's `core::CallKind`: `Lookup`, `Resolve`, `Narrate`, `Summarize`, `Review` and `Renarrate`, the last two appended so that existing seeds keep their values. A turn's second review seeds with its attempt plus `REVIEW_RETRY_OFFSET` (2^23), so it never meets a regenerated turn's first review.
- A cancelled or failed regenerate puts the previous turn back, with the summaries that covered it.

**Cancel and failure:** a cancelled or failed turn commits no turn, a cancel during the review or the renarrate included. Memory upkeep its fit loop finished before the stop (summaries and their history embeddings) is kept, since it only covers committed turns.

**`memory_mode` (per world, overridable in settings).** This setting decides who manages memory. The world sets the default, and an optional `memory_mode` key in `settings.json` overrides it.
- `engine` (default): memory is fully deterministic.
- `assisted`: the model additionally gets:
  - a lookup call before resolving: up to 3 `lookup` requests (lore, history or notes), with results appended to the tail;
  - append-only `notes` in the resolve output.
- Notes are embedded and retrieved (T2), not pinned in L2, so the cached prefix stays stable.

The model never edits or deletes memory.

**Error policy.** Engine bugs and model mistakes are handled differently.
- **Engine bugs** (broken invariants, corrupt data, bad world files): assert or throw, and fail loudly.
- **Model mistakes** (invalid changes, unknown entities, unreadable output, lookups over budget): reject, log and show in the inspector, then continue the turn. Grammars guarantee the output parses, so v1 has no retry loops.
- **A turn that cannot run as asked** (nothing to undo or regenerate, an empty input, an input too long for the tail): `TurnError`, with a message for the player. Nothing changes.

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
- **Budget:** L0-L2 have fixed caps. L3 gets `n_ctx - resolve cap - narrate cap - narrate allowance - tail allowance - templated system message`, where the narrate allowance reserves room for the resolution record and instruction the narrate call appends, and its cost is measured with template tokens included. Overruns are flags for the turn layer (fold, condense, shrink the tail); a context too small for the fixed layers is a settings error.
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14/16 failure; player.location = "deck"]`). Full resolve JSON stays in the database.
- **Digests.** With `compact_transcript` on, only the newest `prose_turns` entries show their narration; an older entry shows its turn's digest (the review's one or two sentences), or its narration when it has none (played with `compact_transcript` off, or its review failed). Each turn, the entry that crosses out of the newest `prose_turns` changes form, so a prefix-reusing model re-evaluates the transcript from there (about `prose_turns + 1` entries); the models without prefix reuse gain from the shorter transcript on every call. Fold calls always see the full narration, and the Play window shows it.
- **Messages.** One system message (L0 + L1 + L2), the transcript as user/assistant pairs (L3), then one user message with the tail (T1-T4). The whole prompt goes through the model's chat template; prefix reuse works at the token level, so no per-layer rendering is needed.
- **Cloud mapping.** The cloud backend maps the same layering onto prompt caching, with a breakpoint at the end of L3.

## Inference interface

```
LlmBackend
  count_tokens(text) -> int
  count_prompt_tokens(prompt) -> int   (the full templated prompt, exactly as complete evaluates it without thinking)
  context_size() -> int
  complete(Prompt{messages, stableCount}, GenerationSpec{grammar?, max_tokens, thinking_max_tokens, sampling}, on_token, cancel)
      -> Completion{text, thinking, tokens_reused, tokens_evaluated, timings}
  embed(texts, purpose: query|document) -> vectors
  embedding_model_id() -> string
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
```

- `LlamaCppBackend` loads two GGUF models in-process: a chat model and an embedding model. It is built with Vulkan.
- `save_state`/`restore_state` carry the cached token list together with the KV sequence state, so prefix reuse still works after a restore.
- The backend takes grammar text from the caller (`GenerationSpec.grammar`, GBNF). The resolve and lookup grammars are generated by `turn::BuildResolveGrammar` / `BuildLookupGrammar`.
- Thinking: a chat model whose template has the `enable_thinking` switch (Qwen3.5 and later) is a thinking model. `llama_chat_apply_template` renders it as plain chatml, so the backend writes the thinking block itself, as the model's template would: each prompt ends with an empty block (`<think>\n\n</think>\n\n`), or with `<think>\n` when `thinking_max_tokens > 0`. The block is sampled without the grammar and is not streamed; when the model ends it, or when the backend closes it at the budget with a short "answer now" sentence, the answer follows under the grammar. `text` holds the answer only and `thinking` the reasoning, which the turn layer keeps in the turn telemetry (shown in the Turn log) and nowhere else. Asking a model without a thinking mode to think is an `InferenceError`. The settings budget a thinking block per call (`resolve_thinking_tokens`, `narrate_thinking_tokens`; 0 is off) and reserve it in the context with the call's answer.
- Grammar sampling checks the sampled token first: the sampler chain picks a token, and only that token is checked against the grammar. Only if the grammar rejects it is the full candidate set masked and the token sampled again. The output always satisfies the grammar.
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type, the token budget (including `retrieval_cap`, the per-call thinking budgets and the review's `review_max_tokens` and `review_thinking_tokens`), `narration_check`, `compact_transcript`, `prose_turns` (at least 1) and an optional `memory_mode`.

## World data and persistence

```
game_projects/<id>/
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode, optional scene;
                      unknown keys are errors
  rules.md            GM instructions -> L1
  lore.csv            subject,data[,always_on]  (RFC 4180)
  state_schema.json   entity types; fields: int{min,max} | enum | string | list<string> | map<item,count>; creatable flag; unknown keys are errors
  initial_state.json
saves/<id>/<run>.db
```

| Table | Contents |
|---|---|
| `run_meta` | world id, world content hash, model id, RNG seed, settings |
| `turns` | n, attempt, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, digest, timings |
| `state_snapshots` | full state JSON after each turn |
| `summaries` | folded turn range and its summary text |
| `notes` | append-only model notes (`assisted` mode), committed in the same transaction as their turn |
| `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |

The schema version is 2 (the `digest` column; it also covers `attempt`). A run written with another version is refused, without migration.

**Scene (optional).** `"scene": {"paths": ["player.location", "crawler.location"], "present_at": "player.location"}` names the string fields (`entity.field`, of entities in the initial state) that say where the player is; `present_at` is one of them. Each turn, `turn::SceneOf` lists the value at each path, then every other entity whose field of `present_at`'s name holds the same value (trimmed, case-insensitive): its id and its other string fields. Values are deduplicated case-insensitively. Retrieval searches with them and boosts the lore they name. Matching is by containment, like the name boost: "the old oasis" names the subject `oasis`, while "Iron Convoy depot" does not name `faction iron convoy`; such cases rely on the embedding.

Changes are validated against the schema: the path exists, the value has the field's type and range, and `create` is only for creatable entity types. Removing more of a counted item than is held is rejected; reaching exactly 0 removes the item.

Rows are append-only, except that undo removes the rows after the target turn. Similarity search is brute-force cosine in memory. Each embedding stores the hash of the text it was computed from, and a changed hash means the vector is re-computed.

World files are read with CRLF normalized to LF. The world content hash covers each file's name, length and text, so a saved run can detect world edits reliably across checkouts.

## UI (ImGui docking windows)

| Window | Shows |
|---|---|
| Runs | Worlds in `game_projects/` and their runs in `saves/<world id>/`: start a new run or continue one |
| Play | Transcript with streaming prose, input box, undo/regenerate, rolls inline |
| State | State tree, with last turn's changes highlighted |
| Context | Last prompt split by layer: tokens per layer, reused vs. evaluated, raw/formatted toggle |
| Turn log | Resolve JSON, rolls, applied and rejected changes, each review's verdict, problems and digest, the narration a renarrate replaced, timings per turn |
| Memory | Summaries, notes, last retrieval hits with scores and reasons, and the turn's scene values |
| Settings | Models (picked from `models/`), context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |

The windows dock over one SDL3 window (Dear ImGui's docking branch, SDL_Renderer backend). The UI thread draws from `app::AppController`, which applies the worker's events once per frame. The backend loads on a background thread, so the window opens at once. Settings apply by restarting the worker: the loaded models are kept unless the model paths, embedding prefixes, context size or GPU layers changed, and the open run reopens.

## Testing (Catch2 v3)

- **`core` unit tests:** validation, change ops, dice. Grammar generation and its tests live in `turn` (`ResolveGrammarTests`).
- **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer.
- **Replay tests:** a `ScriptedBackend` returns recorded completions, so full turns run deterministically and the tests assert on state and database rows. Replay fixtures live in `tests/data/replays` and are recorded from real play with `morpheus_play --record`. A fixture records the run's `narration_check`, `compact_transcript` and `prose_turns` and its `review` replies; a renarrate's reply is among the `narrate` replies, in call order.

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
