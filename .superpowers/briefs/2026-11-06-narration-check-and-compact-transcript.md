# Brief: narration check, compact transcript, lore from the current state

From: MorpheusAmaAgent (read-only Q&A session), 2026-11-06.
To: MorpheusAgent, the implementing session.

## Background
I compared the engine with the seven reference repos in `docs/references/`. The owner picked three ideas to implement, below. Both are design work, not transcription: brainstorm them with the owner, then write a spec and a plan as usual. Below are the problem, the constraints I found in the code, a recommended direction, and the open questions.

## 1. Check the narration against the engine's outcome

### Problem
- The narrate call is told the resolution is final ("never contradict the state", `src/turn/EngineText.h:20`).
- Nothing checks that it obeyed: `Session` stores `narrated.text` as the narration directly (`src/turn/Session.cpp:382-387`).
- So a model can narrate a failed check as a success, or describe a change the engine rejected or never applied. The engine owns the state, but the player sees the prose.
- Reference: freytag-forge validates the narrator's prose against its fact store and rolls the turn back on a violation (`storygame/runtime/validation.py`, `narration_json.py`).

### Constraints
- **Streaming.** The narration has already streamed to the player (`TokenStreamed`) by the time any check runs. A check can flag the narration or replace it, but it cannot stop it being seen.
- **Model mistakes.** Per CLAUDE.md, a contradiction is a model mistake, not an exception: reject or flag it with a reason, log it, and continue the turn.
- **Retry loops.** The spec currently says "v1 has no retry loops", because grammars guarantee the output parses. A re-narrate on contradiction would be the first retry, and that is the owner's call.
- **Context budget.** An extra call must fit like any other: its prompt plus its `max_tokens`. That cost may need a new budget field next to `narrate_allowance`.

### Recommended direction
- After narrate, run a grammar-constrained check call.
  - Prompt: the narrate prompt, plus the narration as an assistant message, plus a short check instruction. The narrate prompt is already cached, so only the narration and the instruction are evaluated.
  - Output grammar: `{"consistent": bool, "problems": [string, ...]}`, with the problems list and lengths bounded the way the resolve grammar bounds its fields.
- Record the verdict in the turn telemetry and show it in the Turn log.
- Settings choose the action: `off`, `flag` (record only) or `renarrate` (one new narrate attempt with the problems appended, replacing the streamed text). This needs a worker event, such as `NarrationRestarted`, so the Play window can clear the streamed text.
- **Deterministic pre-checks, with no model call:**
  - every check outcome word (`success`/`failure`) and every applied change's subject appears in the narration;
  - no rejected change's value appears as if it were applied.
  These are cheap, but they produce false positives on paraphrase, so they should only add hints to the check call, never decide on their own.

### Open questions for the owner
1. Should the check be off, flag or renarrate by default? And should it be per world or in settings.json?
2. Is a second narration that replaces streamed text acceptable in the UI, or should the check only flag?
3. A small local model judging its own prose will miss things. Is flagging good enough as a first step?

### Tests
- **ScriptedBackend fixtures:** a consistent narration passes; a narration that turns a failure into a success is flagged.
- **Renarrate mode:** the replacement narration is the one committed, and the telemetry keeps both verdicts.
- **Budget:** the check call is reserved in the context like the narrate call.
- **Replay fixtures:** they gain the new call kind (`core::CallKind`), so `SamplingSeed` stays distinct.

## 2. Keep the model's own prose out of older transcript entries

### Problem
- L3 replays each past turn's full narration as an assistant message (`src/context/Transcript.cpp:59-69`).
- Models imitate their own recent output, so verbal tics and sentence shapes compound over a run. Long prose also fills L3 quickly, which forces folds sooner.
- Reference: project-lunar feeds condensed facts back instead of raw prose, keeping only the open scene as prose (its `PLANO.md`, `narrative_coherence_plan.md`).

### Constraints
- **Append-only L3.** L3 must stay append-only for KV prefix reuse. If an entry changes form as it ages, the cache is invalidated from that entry on. Only the newest one or two entries may change per turn; then the re-evaluated tail stays small. Measure this with the existing reuse telemetry.
- **Prose anchor.** Keep the latest K turns (K >= 1) as full prose. If every assistant message is a terse digest, the narrate call will start writing digests.
- **Folding.** `Folding.cpp:20` also calls `RenderTranscript` to build the summarization prompt. Folds should probably keep the full narration; decide this explicitly.
- **Saved runs.** A digest stored per turn changes the turns table, which means a `SCHEMA_VERSION` bump in `src/store/RunStore.cpp` (one is already pending).

### Recommended direction
- Give each turn a short digest: the input, the mechanics line, plus one or two sentences of what happened.
- L3 renders turns older than the latest K with the digest as the assistant message; the latest K keep their narration.
- Where the digest comes from, cheapest first:
  - (a) Deterministic: the narration's first sentence or two, cut to N tokens. No call, but crude.
  - (b) A short, grammar-bounded digest call after narrate. It shares the cached narrate prompt and is stored with the turn, so the cost lands once per turn. This could share a call with the narration check from item 1: one call returning both the verdict and the digest.
  - (c) Ask for the digest inside the narrate output. This breaks streaming plain prose, so it is not recommended.
- Expose K, and whether digests are used at all, in settings, so it can be compared by playing.

### Open questions for the owner
1. What should K be by default: 1, 2 or 3?
2. Which digest source: (a), (b), or (b) merged with item 1's check?
3. Should the Play window still show full narration for every turn? I assume yes: digests are for the prompt only.

### Tests
- **RenderTranscript:** with K, older turns render as digests and the newest K as narration.
- **PromptBuilder:** the L3 token counts shrink accordingly.
- **Prefix stability:** a Session test with ScriptedBackend shows `tokens_reused` covers everything before the newest K+1 entries from one turn to the next.
- **Folding:** folds still see the full narration, or whatever the owner decides.
- **Store:** the digest is saved and loaded, and the schema version is bumped.

## 3. Retrieve lore from the current state, not only from the input's wording

### Problem
- Automatic retrieval embeds only the player's input (`src/turn/Session.cpp:280`, `_index.Retrieve(core::CutUtf8(input, ...))`).
- The only exact-match signal is `NAME_BOOST` (+0.15), which applies when the input contains a lore subject as a substring (`src/turn/Retrieval.cpp:125`).
- So the lore for where the player stands, or for the NPCs present, comes in only when the input happens to name them. "I look around" or "I ask her about it" retrieve almost nothing.
- Nothing in the references does this exactly. The closest is coderain's lorebook activation and NarrativeEngine-P's slotted retrieval. The owner asked for this one directly.

### Constraints
- **Where lore lands.** Lore stays in T2 and shares `retrieval_cap` with the other hits (hits are trimmed to it). Nothing moves into the cached start of the prompt.
- **Free-text locations.** Location values are strings the model writes (`player.location = "deck"`), so an exact match against a lore subject will often miss ("the deck" vs "deck"). Any exact match needs normalization, plus the embedding as a fallback.
- **The world decides which paths count.** That means a declaration in `world.json`, not engine knowledge of `location`. `world.json` rejects unknown keys, so this is a format change with loader tests.
- **Determinism.** Retrieval is already deterministic, given the embeddings; keep it so (replay fixtures depend on it).

### Recommended direction
- `world.json` gains an optional list of state paths that give the scene, e.g. `"scene_paths": ["player.location", "crawler.location"]`.
- Each turn, the engine reads those values from the live state, plus the ids (or `role`s) of entities whose location equals the player's. Then:
  - (a) Lore entries whose lower-cased subject equals a normalized value qualify as hits, with a boost like `NAME_BOOST`, without the input naming them.
  - (b) The scene values are appended to the retrieval query text, so the embedding search also leans toward the current scene. This covers near-miss strings that (a) misses.
- The telemetry tags each hit with why it was picked (input similarity, name, scene), and the Memory window shows that.

### Related (decide in the same pass)
The query could also take the previous turn's narration, or its last sentence, to resolve pronouns ("ask her about it"). It is the same mechanism (what goes into the query). The owner has not chosen this yet; ask.

### Open questions for the owner
1. Should scene paths be declared per world (recommended) or fixed by the engine?
2. Should scene hits be guaranteed a slot within `retrieval_cap`, or compete on score with the boost?
3. Should NPCs present at the player's location count as scene values too?

### Tests
- **Retrieval:** a scene value equal to a lore subject retrieves it with an input that names nothing; normalization ("The Deck" vs "deck").
- **Ranking:** scene hits are still trimmed to `retrieval_cap` and ranked with the others.
- **World loading:** `scene_paths` loads; an unknown path, or a path that is not a string field, is a DataError naming `world.json`.
- **Session with ScriptedBackend:** moving the player changes which lore reaches the resolve prompt.
