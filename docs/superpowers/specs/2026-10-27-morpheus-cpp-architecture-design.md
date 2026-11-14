# MorpheusEngine (C++) - Target Architecture

Date: 2026-10-27. Status: approved design, pre-implementation.

## Goal and constraints

A text-based RPG engine driven by a local LLM, built first for the author to play their own worlds.

- **Local-first.** llama.cpp runs inside the engine process, or the models are reached through `llama-server` over HTTP (`settings.json`'s `backend`), so one model host can serve several consumers. A cloud provider can be added behind the same interface later.
- **Runs on the author's laptop.** The machine is an RTX 3070 Laptop GPU (8 GB VRAM) with 32 GB RAM. That means quantized GGUF chat models of roughly 4-12B parameters with 16k+ context, some layers offloaded to the CPU where needed. The chat model should be multimodal, so it can also read images (see the visualization spec). A larger GPU only removes model swapping and allows bigger models; nothing is redesigned.
- **The engine owns the game.** It holds structured state and rolls the dice. The LLM proposes outcomes and narrates them.
- **C++20, one process, one executable.** No HTTP between modules, no router, no Ollama. The only HTTP is the optional llama-server backend's, to a model host the owner runs.

## Process and modules

Two threads talk through two thread-safe queues (and, for the model seat, a third channel: a mutex-guarded state the UI thread and the worker share):

- **UI thread:** the ImGui render loop. It never blocks on the worker; the model seat's Check blocks it briefly on the tokenizer and grammar matcher.
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time. The one exception is the model seat's Check (a debug feature of the app): it calls `grammar::Check` and the backend's `CountTokens` from the UI thread, only while the worker is blocked waiting for the seat.
- **Commands (UI to worker):** `StartRun`, `LoadRun`, `SubmitInput`, `UndoTurn`, `RegenerateTurn`. Cancel is `Worker::Cancel`, not a command: it interrupts the running command, and a command cancelled before it runs closes with `TurnCancelled` without running.
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `NarrationRestarted`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window. `NarrationRestarted` carries the review's problems and says that the running turn's narration starts over: the `TokenStreamed` events after it are the new narration.
- Every command ends with exactly one closing event. `TurnResolved`, `TokenStreamed` and `NarrationRestarted` report a running turn's progress; `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep. None of them close a command.

Each module is a static library. Dependencies flow downward only.

| Module | Responsibility | Depends on |
|---|---|---|
| `core` | World definition, schema-typed state, change validation, dice, turn record types, settings. Pure logic; its only I/O is reading world folders and settings.json. | - |
| `store` | Per-run SQLite: turns, state snapshots, summaries, notes, embeddings; for hosted runs the host data, conversations, hosted turns and the events between them | core |
| `inference` | The `Tokenizer`, `ChatModel` and `Embedder` interfaces, `LlmBackend` (their union), prompt types and the thinking block's texts (no llama.cpp, no HTTP) | - (only `core/Errors.h`, header-only) |
| `llama` | `LlamaCppBackend`: tokenize, KV prefix reuse, grammar sampling, embeddings, state save/restore. The only module linking llama.cpp; built by the `dev` and `linux-release` presets only. | inference |
| `llamaserver` | `LlamaServerBackend`: the models through `llama-server` over HTTP (cpp-httplib, no OpenSSL): token counts, prompt rendering, streamed completions on a slot with prompt-cache reuse, embeddings. Built in every preset, behind `MORPHEUS_BUILD_LLAMA_SERVER` (default on) | inference, core |
| `context` | Layered prompt assembly (the owned resolve prompt and the hosted prompt, on rendered transcript entries), exact token budgets, cache-prefix tracking | core, inference |
| `grammar` | Bounded-JSON grammars for llama.cpp: the GBNF builder (`GrammarBuilder` and its expressions), `FromSchema` for a subset of JSON Schema, `Validate`, which checks a value against the same subset, and `grammar::Check`, which says whether a text fits a GBNF grammar and where it stops fitting. No llama.cpp | core |
| `turn` | Turn orchestration (grammar, resolve and apply, retrieval, folding and the shared memory upkeep, session), hosted runs (`HostedWorld`, `HostedSession`), the worker thread, replay recording | core, store, inference, context, grammar |
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings, the model seat | turn, llama and llamaserver (`main.cpp`'s backend factories) |

**Grammars:** `grammar` turns a bounded JSON shape into GBNF for llama.cpp (design: `docs/superpowers/specs/2026-11-14-grammar-library-design.md`). Its builder writes compact JSON with no optional whitespace: literals, bounded JSON strings, exact integer ranges (`IntRange`, any `long long` range, no leading zeros or `-0`), enums, bounded lists, objects with fixed keys in a fixed order and alternatives; `GrammarBuilder` collects them into named rules and writes the root line first, because `turn::CallKindOf` recognises a lookup and a review call by their grammar's first line. Two limits come from llama.cpp's grammar loader, which refuses a repetition count or a repetition that creates too many rules: a string holds at most 1999 characters (`GBNF_MAX_STRING_CHARS`) and a list at most 1000 items (`GBNF_MAX_LIST_ITEMS`), and a list's element must be a rule reference or one string literal (`FromSchema` names array items `items-N`); the builder throws `ContractError` above them. `FromSchema` covers a subset of JSON Schema: objects whose keys are all required (written in `required` order, no extra keys), `enum` and `const` of strings or integers, bounded integers, strings and arrays, and `oneOf` with a discriminator key; anything else, and a `maxLength` or `maxItems` above the two limits, is a `DataError` naming the JSON pointer inside the schema. `Validate` checks a value against the same subset, extended for schemas that are only validated (numbers with optional bounds, booleans, optional keys, open objects with `additionalProperties: true`, strings and arrays without an upper limit, and none of llama.cpp's limits; `FromSchema` still refuses each, and `CheckValidationSchema` checks such a schema without a value), and returns its mistakes as data, a JSON pointer into the value and a message each; it compares integers by their written form, so a huge unsigned never equals a negative. `PointerToken` (public) escapes one key as a JSON pointer token, for callers that build pointers of their own. Morpheus's own resolve, lookup and review grammars (`turn`) are built on the builder, not on JSON Schema. `grammar::Check(grammar, text)` matches a text against a grammar by code point, as llama.cpp's grammar sampler does, memoised by (expression, position) so long texts stay fast; it returns "" or llama.cpp's reason texts ("the grammar rejects '<c>' at the start", "... after '<text>'", "the text ends before the grammar does"), pointing at the furthest position any partial match reached, and throws `ContractError` for a grammar it cannot read. The model seat checks replies with it; the `[llama]` tests check that it agrees with llama.cpp.

**Build:** CMake, with dependencies pulled through FetchContent and pinned to commit SHAs: llama.cpp, nlohmann/json, SDL3, ImGui (upstream's docking branch, v1.92.8-docking), the SQLite amalgamation, Catch2 v3, cpp-httplib (header-only, without OpenSSL; Morpheus defines its own `morpheus_httplib` target). llama.cpp's GPU backend is a choice, `MORPHEUS_GPU_BACKEND`: `Vulkan` (the default), `CUDA` or `CPU`. Warnings are treated as errors (`/W4 /permissive-`, `MORPHEUS_WARNINGS_AS_ERRORS`) when Morpheus is the top-level project. Included in another project through FetchContent, it builds by default only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn`, `Morpheus::llamaserver`) with nlohmann/json (unless the host provides it), SQLite (`morpheus_sqlite3`) and cpp-httplib's header (`morpheus_httplib`; `MORPHEUS_BUILD_LLAMA_SERVER` off leaves it and the llama-server backend out): no llama.cpp, SDL3, ImGui or Catch2, warnings that are not errors, and the host's output directories; `cmake/consumer-check/` checks this. AddressSanitizer is opt-in: the `asan` and `dev-asan` presets. On Linux, the `linux-tests` and `linux-release` (CUDA) presets.

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
Tokenizer
  count_tokens(text) -> int
  count_prompt_tokens(prompt) -> int   (the full templated prompt, exactly as complete evaluates it without thinking)
  context_size() -> int
ChatModel
  complete(Prompt{messages, stableCount}, GenerationSpec{grammar?, max_tokens, thinking_max_tokens, sampling, slot}, on_token, cancel)
      -> Completion{text, thinking, tokens_reused, tokens_evaluated, timings}
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
Embedder
  embed(texts, purpose: query|document) -> vectors
  embedding_model_id() -> string
LlmBackend = Tokenizer + ChatModel + Embedder
```

- Callers that need the whole backend take `LlmBackend`; code that needs one part (sub-project D's server) can take `Tokenizer`, `ChatModel` or `Embedder` alone. `GenerationSpec::slot` names the server slot whose prompt cache a call uses: -1 (the default, Morpheus's own game) is the backend's default slot; the in-process backend has one cache and ignores it. `turn::SamplingFrom` builds a call's sampling from `settings.json`. The thinking block's texts (`inference::THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT`, `CLOSE_THINKING_TEXT`) are shared by both backends.

- `LlamaCppBackend` loads two GGUF models in-process: a chat model and an embedding model. llama.cpp is built with the GPU backend `MORPHEUS_GPU_BACKEND` names: Vulkan (the default), CUDA or the CPU.
- `save_state`/`restore_state` carry the cached token list together with the KV sequence state, so prefix reuse still works after a restore.
- The backend takes grammar text from the caller (`GenerationSpec.grammar`, GBNF). The resolve and lookup grammars are generated by `turn::BuildResolveGrammar` / `BuildLookupGrammar`.
- Thinking: a chat model whose template has the `enable_thinking` switch (Qwen3.5 and later) is a thinking model. `llama_chat_apply_template` renders it as plain chatml, so the backend writes the thinking block itself, as the model's template would: each prompt ends with an empty block (`<think>\n\n</think>\n\n`), or with `<think>\n` when `thinking_max_tokens > 0`. The block is sampled without the grammar and is not streamed; when the model ends it, or when the backend closes it at the budget with a short "answer now" sentence, the answer follows under the grammar. `text` holds the answer only and `thinking` the reasoning, which the turn layer keeps in the turn telemetry (shown in the Turn log) and nowhere else. Asking a model without a thinking mode to think is an `InferenceError`. The settings budget a thinking block per call (`resolve_thinking_tokens`, `narrate_thinking_tokens`; 0 is off) and reserve it in the context with the call's answer.
- Grammar sampling checks the sampled token first: the sampler chain picks a token, and only that token is checked against the grammar. Only if the grammar rejects it is the full candidate set masked and the token sampled again. The output always satisfies the grammar.
- `LlamaCppBackend::CheckGrammar` (a plain method, not part of the interface) generates nothing: it tokenizes the text as an answer and runs llama.cpp's grammar sampler over it, checking each token alone and then accepting it, and at the end requires the end-of-generation token to be allowed. Its reasons have `grammar::Check`'s shape, naming the first rejected token instead of a code point. An empty or unparsable grammar is a `ContractError`. The `[llama]` tests use it to confirm that `grammar::Check`, which the app's model seat uses, agrees with llama.cpp.
- `LlamaServerBackend` (`llamaserver`, design: `docs/superpowers/specs/2026-11-15-llama-server-backend-design.md`) reaches the models through `llama-server`. Its URLs are validated at construction (plain `http://host` or `http://host:port`; IPv6 literal hosts are refused). It keeps one connection alive to each server for the non-streaming endpoints and a dedicated kept connection for `/completion` (so a cancel closes only that one); a non-streaming request, and a `/completion` that was not answered, is sent once more when a stale socket failed it (llama-server closes idle keep-alive connections after 5 s). At startup it reads both servers' `GET /props`: the chat server's per-slot context must hold `context_size`, its chat template tells whether the model thinks, and the embedding server's model file names the vectors. It counts tokens with `POST /tokenize`, renders prompts with `POST /apply-template` around content markers and tokenizes each template piece (special tokens parsed) and each content (plain text) apart, as in-process (`inference::ContentMarker`, `MarkContents`, `TemplatePieces`), appending the thinking markers as tokens, and streams `POST /completion` with the prompt as token ids, `id_slot`, `cache_prompt` and the in-process sampling chain (`samplers` top_k, top_p, temperature; every other server sampler neutral); the server's `timings` fill the telemetry. A call that thinks is two calls on one slot: the block (stopping at `</think>` or its budget), then the answer, whose tokens extend the first call's (the thinking text tokenized again as plain text) so the server reuses its cache. A stop closes the connection and llama-server cancels the task. It embeds with `POST /embedding`, one text at a time. `SaveState` and `RestoreState` are not supported (`InferenceError`). One thread owns each backend; several may share a server, each on its own slot.
- `settings.json` holds the backend (`"llama-cpp"`: the model paths and the number of GPU layers; `"llama-server"`: a `server` object with `chat_url`, `embedding_url` and `slot`; each backend's keys are ignored under the other), `context_size`, sampling per call type, the token budget (including `retrieval_cap`, the per-call thinking budgets and the review's `review_max_tokens` and `review_thinking_tokens`), `narration_check`, `compact_transcript`, `prose_turns` (at least 1) and an optional `memory_mode`.

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
| `run_meta` | world id, world content hash, model id, RNG seed, settings, kind (`owned` or `hosted`) |
| `turns` | n, attempt, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, digest, timings |
| `state_snapshots` | full state JSON after each turn |
| `summaries` | folded turn range and its summary text |
| `notes` | append-only model notes (`assisted` mode), committed in the same transaction as their turn |
| `embeddings` | kind, ref id, embedding model, content hash of the embedded text, vector |
| `host_data` | hosted runs: the host's action catalog, observation schema and lore as one JSON document (its hash is `run_meta`'s world hash) |
| `conversations` | hosted runs: id (`c1`, `c2`, ...), participants with their names and bios in join order, ended, folded through |
| `hosted_turns` | hosted runs: n (run-wide), conversation, speaker, listeners, player text, the request's events, observation, resolve output as written, adjudication, line, timings |
| `conversation_events` | hosted runs: events recorded between turns, in order, with the conversation's turn they follow |

The schema version is 3 (`run_meta.kind` and the hosted tables; version 2 added the `digest` column and covers `attempt`). Every file has every table. A version 2 run is upgraded in place on open: the hosted tables are added and it is marked owned, in one transaction, its data untouched. A run written with any other version is refused. Each kind has its own creator (`Create` and `CreateHosted`), and the writers of one kind refuse a run of the other with a `ContractError`. Stored hosted data is shape-checked on read: a column that is not what the writer wrote is a `StoreError` naming the file and the column; a hosted turn's observation must keep the actors core, with the turn's speaker among them. The actors core has one reader, `core::ReadActors` (`core/HostedRecord.h`), which returns the actors and every fault without throwing: `HostedWorld::Observe` turns its faults into a `DataError`, `RunStore::HostedTurns` into a `StoreError`.

**Scene (optional).** `"scene": {"paths": ["player.location", "crawler.location"], "present_at": "player.location"}` names the string fields (`entity.field`, of entities in the initial state) that say where the player is; `present_at` is one of them. Each turn, `turn::SceneOf` lists the value at each path, then every other entity whose field of `present_at`'s name holds the same value (trimmed, case-insensitive): its id and its other string fields. Values are deduplicated case-insensitively. Retrieval searches with them and boosts the lore they name. Matching is by containment, like the name boost: "the old oasis" names the subject `oasis`, while "Iron Convoy depot" does not name `faction iron convoy`; such cases rely on the embedding.

Changes are validated against the schema: the path exists, the value has the field's type and range, and `create` is only for creatable entity types. Removing more of a counted item than is held is rejected; reaching exactly 0 removes the item.

Rows are append-only, except that undo removes the rows after the target turn. Similarity search is brute-force cosine in memory. Each embedding stores the hash of the text it was computed from, and a changed hash means the vector is re-computed.

World files are read with CRLF normalized to LF. The world content hash covers each file's name, length and text, so a saved run can detect world edits reliably across checkouts.

## Hosted runs

Morpheus can run a world it does not own (design: `docs/superpowers/specs/2026-11-16-hosted-world-design.md`; fixture format: `tests/hosted/fixtures/README.md`). The host sends once per run an action catalog, the JSON Schema of its observation and optional lore (`turn::HostData`), and with each request the observation, events and who speaks. Morpheus never simulates the world: an accepted action is requested, not applied, and its outcome comes back later as an event.

- **`turn::HostedWorld`** checks the host data (a `DataError` naming the JSON pointer of the first fault) and holds the three operations over it: `Observe`, `ProposalGrammar` (with `ProposalSchema`) and `Adjudicate`. `Observe` validates an observation: first a fixed core the engine reads whatever the schema says (`actors`, each with a non-empty `id` and `name`, `is_player` and an optional `bio`; ids unique; exactly one player), then `grammar::Validate` against the host's schema, which must be in the extended subset; every fault is reported. `ProposalSchema` / `ProposalGrammar` build the speaker's possible actions for one turn: a `oneOf` over the allowed actions in catalog order, discriminated by `"action"`, each `{"action": <id>, "params": {...}}` with every parameter required and no extra keys; an actor parameter is an enum of the ids its scope gives (`present`, `present_non_player`, `participant`), never the speaker, and an action with no one to name is left out. `Adjudicate` parses the model's text and validates it against the turn's proposal schema: a valid action is requested, anything else (including text the JSON parser refuses, such as lone surrogate escapes and numbers it cannot hold, whatever the exception) is refused with its reasons; it never throws for the model's text. The session builds the proposal schema and its grammar once per turn.
- **`turn::HostedSession`** is one open hosted run in the same `RunStore` (kind `hosted`, world id `hosted`). `Open` creates the run or reopens it; with changed host data (a mod update) it re-embeds changed lore and only then replaces the stored copy, so a failed open leaves it unchanged; an owned file is refused. Conversations start with participants (their names and bios from the observation, in join order), take events between turns, gain and lose participants, and end. `RunTurn` checks the request (no model call before it passes; the player's text is cleaned by `turn::CleanInput`, which `Session` uses too), retrieves from lore and the run's folded turns, resolves under the proposal grammar, reports the adjudication through `onAction`, speaks on a prompt that extends the resolve prompt (streamed through `onToken`, no grammar), and commits the hosted turn record; a cancel commits nothing. There is no review, no dice, no state and no snapshot. Every event recorded since a conversation's last turn joins its next turn's tail; when they alone are over `tail_allowance`, every later turn of that conversation is a `TurnError` until it ends. D1's contract: one `HostedSession` per run file (two on one file are not supported); it is not thread-safe, so its caller serializes every call on a run, turns of different conversations included, and `Maintain` runs between them and stops through its stop token; every model call uses the backend's default slot. A slot per conversation and any concurrency across conversations are left to D2 (`morpheus_server`), which can add them without changing this API. `Session::Load` refuses a hosted run's file, as `HostedSession::Open` refuses an owned one.
- **The hosted prompt** (`context::AssembleHostedPrompt`), most stable first: L0 the hosted system text (`EngineText`, generic, no variable), L1 the host's always-on lore, the participants (`- <name> (<id>): <bio>`, or `- <name> (<id>)` for one without a bio, in join order; a join or a leave changes the prompt from here on), L2 the run-wide summaries, L3 the conversation's unfolded turns as user/assistant pairs (the events in parentheses and the player's line, or the cue `(<speaker> speaks.)` when a turn has neither; then `<speaker> [<action id> <params>]: <line>`, the params only when the action has any; a refused action has no bracket), then the tail: retrieved snippets, the observation as compact JSON, the new events, the player's line and the resolve instruction. Budgets and flags are the owned prompt's; `world_cap` bounds the lore and the participants together. The prompt of a conversation's next turn starts with this turn's stable part, byte for byte, unless a join or a leave or a fold landed in between: summaries are run-wide, so a fold in any conversation (during a turn or in `Maintain`) changes every open conversation's system message from L2 on, while L0, L1 and the participants stay. The owned and the hosted builders share the rendering of the always-on lore and of the summaries (`RenderAlwaysOnLore`, `RenderSummaries`), so L1 and L2 read alike in both.
- **The shared memory upkeep** (`turn::MemoryUpkeep`) is `Session`'s fit loop, fold and condense, working on summaries and rendered transcript entries (`context::TranscriptEntry`: a turn number, its L3 messages, its fold messages and a one-line fact), so `Session` passes its turns and `HostedSession` its conversation's. During a conversation, an L3 over its budget folds its oldest turns into a run-wide summary; after `EndConversation`, `HostedSession::Maintain` folds every remaining turn (in one call when it fits). Folded turns of every conversation are retrievable history, under their run-wide turn numbers.

## UI (ImGui docking windows)

| Window | Shows |
|---|---|
| Runs | Worlds in `game_projects/` and their runs in `saves/<world id>/`: start a new run or continue one |
| Play | Transcript with streaming prose, input box, undo/regenerate, rolls inline |
| State | State tree, with last turn's changes highlighted |
| Context | Last prompt split by layer: tokens per layer, reused vs. evaluated, raw/formatted toggle |
| Turn log | Resolve JSON, rolls, applied and rejected changes, each review's verdict, problems and digest, the narration a renarrate replaced, timings per turn |
| Memory | Summaries, notes, last retrieval hits with scores and reasons, and the turn's scene values |
| Settings | Backend (llama.cpp in-process, or llama-server over HTTP); models (picked from `models/`) or the server URLs and slot; context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |
| Model seat | Debug: answer the engine's model calls by hand. "Answer as the model" and a Human / Draft / Model mode per call label (resolve, narrate, rewrite, lookup, review, summarize, condense); the waiting call's label, limits, prompt by layer, grammar and the model's draft; reply and thinking boxes, Check (token count and grammar fit), Submit, and Withdraw (releases the call: cancels a running turn, or ends a between-turn fold or condense) |

The windows dock over one SDL3 window (Dear ImGui's docking branch, SDL_Renderer backend). The UI thread draws from `app::AppController`, which applies the worker's events once per frame. The backend loads on a background thread, so the window opens at once. Settings apply by restarting the worker: the loaded backend is kept unless the backend kind, the model paths, the server URLs or slot, the embedding prefixes, the context size or GPU layers changed, and the open run reopens. `app::BackendFactoryByKind` makes the backend `settings.json` names.

The model seat (design: `docs/superpowers/specs/2026-11-13-model-seat-design.md`): `AppController` owns one `app::ModelSeat` and wraps every backend the engine host builds in an `app::ModelSeatBackend` around it, so the switch and the modes survive restarts; nothing is stored in `settings.json`. With the seat off, or a call's label in Model mode, the wrapper forwards. Otherwise the call waits on the worker thread for the owner's reply (in Draft mode after the real model's answer, offered as a draft); the reply is cut to the call's limits by the real tokenizer and returned as the model's completion, streamed once, whole. A cancel, or the window's Withdraw, wakes the wait and the call ends cancelled; the UI names the request it answers by its id, so a click that loses the race with a cancel is a no-op. The reply is not constrained by the grammar, so the engine's handling of model mistakes applies to it; `core`, `context` and `turn` do not know who answers.

## Testing (Catch2 v3)

- **`core` unit tests:** validation, change ops, dice.
- **`grammar` tests:** the builder's text, `IntRange` checked over windows of many ranges and at the edges of `long long`, every rule of `FromSchema` and `Validate`, and a property test: 1000 random values drawn from a schema with a fixed seed pass `Validate` and the schema's grammar. The fast suite matches grammars with `grammar::Check` (whose own tests cover its reasons, its strict repetition counts and its memoisation); the `[llama]` suite checks the same values with llama.cpp's grammar sampler, and that both agree on accept, reject and the kind of reason. Morpheus's grammars are tested in `turn` (`ResolveGrammarTests`), including what they accept and reject.
- **`context` tests:** layer assembly, budgets and folding triggers, using a fake tokenizer; the hosted layers, their budgets and their stable prefix across turns and joins.
- **Hosted tests:** `HostedWorld` (the proposal schema and its grammar, actor scopes, allowed actions, each fault of the host data and the observation core, `Adjudicate`), `HostedSession` (open, reopen and host data updates, conversations, turns, cancel, each request error, folding during a conversation and after its end) and the store's version 3, its upgrade and its reads of damaged hosted data, all with the fake backend. Fixtures in `tests/hosted/fixtures/` are written from the integration requirements' data contracts: the host data, an observation, a scripted conversation, a radiant conversation of two NPCs and the requests that must fail; the host's recorded fixtures can replace them. One `[llama]` test plays a hosted conversation on the real model.
- **`llamaserver` tests:** a fake `llama-server` (`tests/fakes/FakeLlamaServer.h`, a cpp-httplib server on a free local port) answers from a script and records every request: startup, token counts, rendering and its thinking markers, the `/completion` body, streaming, telemetry, thinking in two calls, errors, cancel and embeddings. The `[server]` suite runs against a `llama-server` the owner starts (`MORPHEUS_TEST_SERVER_URL`, `MORPHEUS_TEST_EMBED_SERVER_URL`), including R2's three sessions on three slots, which reports each call's reused share of its prompt.
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

- Multiple processes of Morpheus's own, a router/proxy, allowlists, aliases, and JSON pipeline templates (the llama-server backend's HTTP goes to a model host Morpheus does not run)
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
