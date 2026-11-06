### Task 10: Documentation and milestone check

The architecture spec and README describe the review, the renarrate, the compact transcript and scene retrieval. `CLAUDE.md` does not change. A last full run under AddressSanitizer closes the milestone.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `README.md`

**Interfaces:**
- Consumes: every name above.
- Produces: documentation only.

- [ ] **Step 1: Update the architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md` (read it first; each old text below is unique):

Replace

```markdown
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window.
- Every command ends with exactly one closing event. `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep and close nothing.
```

with

```markdown
- **Events (worker to UI):** `RunOpened`, `TokenStreamed`, `NarrationRestarted`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MemoryUpdated`, `MaintenanceFailed`, `Error`. `RunOpened`, `TurnCompleted` and `StateChanged` carry the run's memory (summaries and notes) for the Memory window. `NarrationRestarted` carries the review's problems and says that the running turn's narration starts over: the `TokenStreamed` events after it are the new narration.
- Every command ends with exactly one closing event. `TurnResolved`, `TokenStreamed` and `NarrationRestarted` report a running turn's progress; `MemoryUpdated` (maintenance finished) and `MaintenanceFailed` report between-turn upkeep. None of them close a command.
```

Replace

```markdown
Embed the player input. Rank lore entries, folded past turns and (in `assisted` mode) notes by cosine similarity, boosting lore subjects named in the input.
```

with

```markdown
Embed the query: the player input, the current scene's values joined by `; ` (see "World data"), then the previous turn's digest, one per line and cut to 4000 bytes with the input first. Rank lore entries, folded past turns and (in `assisted` mode) notes by cosine similarity, adding 0.15 once for a lore subject named in the input or in a scene value (the hit's reasons: `name`, `scene`); the digest only enters the embedding. Assisted-mode lookups search with their own query only.
```

Replace

```markdown
5. **Commit (engine).** Write the turn row, the state snapshot and any `assisted`-mode notes in one transaction.
```

with

```markdown
5. **Review (LLM, JSON grammar), when `narration_check` or `compact_transcript` is on.** A standalone call, not an extension of the narrate prompt: the review rules as the system message, then the player input, the outcome rendered as the narrate message renders it, and the narration. It is about 300 to 600 tokens whatever the run's length, so it needs no reservation in the context fit. Its grammar (`turn::BuildReviewGrammar`) allows `consistent` and up to 3 `problems` of up to 200 characters (the check) and a `digest` of up to 300 characters (the compact transcript). Output that cannot be read (cut off at `review_max_tokens`) is a model mistake: the turn goes on as if the review had found nothing. On Qwen3-4B-Instruct the review prompt replaces the narrate prompt in the backend's cache, so the next resolve call evaluates its whole prompt; Qwen3.5 and later reuse no prefix anyway.
6. **Renarrate (LLM, streamed prose), at most once, with the check on.** When the first review says the narration contradicts the outcome (a failed check narrated as a success or the reverse, an applied change left out or reversed, a rejected change narrated as if it happened), the worker sends `NarrationRestarted` and the narration is written again from the same narrate prompt, whose narrate message gains the review's problems before the narrate instruction. It stays within `narrate_allowance`, the problem lines shed last. The second narration is reviewed, and it and that review's digest are committed whatever the verdict: there are no loops.
7. **Commit (engine).** Write the turn row (with the last review's digest), the state snapshot and any `assisted`-mode notes in one transaction.
```

Replace

```markdown
- Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number).
```

with

```markdown
- Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number). `SamplingSeed` also takes the call's `core::CallKind`: `Lookup`, `Resolve`, `Narrate`, `Summarize`, `Review` and `Renarrate`, the last two appended so that existing seeds keep their values. A turn's second review seeds with its attempt plus `REVIEW_RETRY_OFFSET` (2^23), so it never meets a regenerated turn's first review.
```

Replace

```markdown
**Cancel and failure:** a cancelled or failed turn commits no turn.
```

with

```markdown
**Cancel and failure:** a cancelled or failed turn commits no turn, a cancel during the review or the renarrate included.
```

Replace

```markdown
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14/16 failure; player.location = "deck"]`). Full resolve JSON stays in the database.
```

with

```markdown
- **Compact transcript entries.** Each L3 entry is the input, the narration, and one mechanics line (`[agility vs hard: 14/16 failure; player.location = "deck"]`). Full resolve JSON stays in the database.
- **Digests.** With `compact_transcript` on, only the newest `prose_turns` entries show their narration; an older entry shows its turn's digest (the review's one or two sentences), or its narration when it has none (played with `compact_transcript` off, or its review failed). Each turn, the entry that crosses out of the newest `prose_turns` changes form, so a prefix-reusing model re-evaluates the transcript from there (about `prose_turns + 1` entries); the models without prefix reuse gain from the shorter transcript on every call. Fold calls always see the full narration, and the Play window shows it.
```

Replace

```markdown
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type, the token budget (including `retrieval_cap` and the per-call thinking budgets) and an optional `memory_mode`.
```

with

```markdown
- `settings.json` holds the model paths, `n_ctx`, the number of GPU layers, sampling per call type, the token budget (including `retrieval_cap`, the per-call thinking budgets and the review's `review_max_tokens` and `review_thinking_tokens`), `narration_check`, `compact_transcript`, `prose_turns` (at least 1) and an optional `memory_mode`.
```

Replace

```markdown
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode; unknown keys are errors
```

with

```markdown
  world.json          id, title, roll expression (e.g. 1d20+stat), check entity, stats (the check entity's
                      int fields usable in checks), difficulty table, default memory_mode, optional scene;
                      unknown keys are errors
```

Replace

```markdown
| `turns` | n, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, timings |
```

with

```markdown
| `turns` | n, attempt, input, resolve JSON, checks (typed check outcomes), applied / rejected changes, narration, digest, timings |
```

After the table row `| \`embeddings\` | kind, ref id, embedding model, content hash of the embedded text, vector |` and the blank line that follows it, insert:

```markdown
The schema version is 2 (the `digest` column; it also covers `attempt`). A run written with another version is refused, without migration.

**Scene (optional).** `"scene": {"paths": ["player.location", "crawler.location"], "present_at": "player.location"}` names the string fields (`entity.field`, of entities in the initial state) that say where the player is; `present_at` is one of them. Each turn, `turn::SceneOf` lists the value at each path, then every other entity whose field of `present_at`'s name holds the same value (trimmed, case-insensitive): its id and its other string fields. Values are deduplicated case-insensitively. Retrieval searches with them and boosts the lore they name. Matching is by containment, like the name boost: "the old oasis" names the subject `oasis`, while "Iron Convoy depot" does not name `faction iron convoy`; such cases rely on the embedding.

```

Replace the three UI table rows

```markdown
| Turn log | Resolve JSON, rolls, applied and rejected changes, timings per turn |
| Memory | Summaries, notes, and last retrieval hits with scores |
| Settings | Models (picked from `models/`), context size, thinking budgets, sampling, `memory_mode` |
```

with

```markdown
| Turn log | Resolve JSON, rolls, applied and rejected changes, each review's verdict, problems and digest, the narration a renarrate replaced, timings per turn |
| Memory | Summaries, notes, last retrieval hits with scores and reasons, and the turn's scene values |
| Settings | Models (picked from `models/`), context size, thinking budgets (the review's included), sampling, `memory_mode`, narration check, compact transcript and prose turns |
```

Replace

```markdown
Replay fixtures live in `tests/data/replays` and are recorded from real play with `morpheus_play --record`.
```

with

```markdown
Replay fixtures live in `tests/data/replays` and are recorded from real play with `morpheus_play --record`. A fixture records the run's `narration_check`, `compact_transcript` and `prose_turns` and its `review` replies; a renarrate's reply is among the `narrate` replies, in call order.
```

- [ ] **Step 2: Update the README**

In `README.md` (read it first):

Before the bullet that starts ``- `gpu_layers` is how many of the chat model's layers``, insert:

```markdown
- `narration_check` (true or false): after each narration a short review call checks it against the engine's outcome. A narration that contradicts it (a failed check told as a success, a change left out or reversed, a rejected change told as if it happened) is written once more and replaces the first.
- `compact_transcript` (true or false) and `prose_turns` (at least 1): the review also writes a one or two sentence digest of each turn, and in the prompt only the newest `prose_turns` turns keep their full narration; older ones show the digest. The model then copies its own prose less, and the transcript grows more slowly. The Play window always shows the full story.
- `budget.review_max_tokens` and `budget.review_thinking_tokens` are the review call's answer and thinking budgets (0, or at least 64, like the others). The review prompt is short and standalone, so they are not reserved in the turn prompt's context.
```

Replace

```markdown
Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn.
```

with

```markdown
Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn. When the review finds that a narration contradicts the outcome, the text clears and the rewritten narration streams in its place.
```

Replace

```markdown
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups and per-call token counts and timings, and the raw resolve output.
```

with

```markdown
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups, the narration review (each review's verdict, problems and digest, and the narration a rewrite replaced), per-call token counts and timings, and the raw resolve output.
```

Replace

```markdown
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores.
```

with

```markdown
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores and why each hit matched (`name`: named in your input; `scene`: named by the scene), and the scene values it searched with.
```

Replace

```markdown
context size, GPU layers, the resolve and narrate thinking budgets (0 for a model without a thinking mode), sampling and memory mode.
```

with

```markdown
context size, GPU layers, the resolve, narrate and review thinking budgets (0 for a model without a thinking mode), sampling, memory mode, the narration check, the compact transcript and its prose turns.
```

After the line ``- `/quit`: leave``, insert a blank line and:

```markdown
When the review finds that a narration contradicts the outcome, the terminal prints `[narration rewritten: <problems>]` and streams the new narration below it.
```

Before the line `## Layout`, insert:

````markdown
## Worlds

A world is a folder `game_projects/<id>/`: `world.json` (id, title, roll expression, check entity, stats, difficulties, default memory mode), `rules.md`, `lore.csv`, `state_schema.json` and `initial_state.json`. `world.json` may also declare a scene, the state values that say where the player is:

```json
"scene": {
  "paths": ["player.location", "crawler.location"],
  "present_at": "player.location"
}
```

Each path is `entity.field`, a string field of an entity in `initial_state.json`, and `present_at` is one of the paths. Every turn, retrieval also searches with those values and with the entities at `present_at`'s value (an NPC standing where the player stands, with its other string fields, such as its role), so lore about the place reaches the prompt even when the input names nothing. A world without a scene searches with the input and the previous turn's digest.

````

- [ ] **Step 3: Check the whole milestone**

Run: `ctest --preset tests`
Expected: all pass.

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: all pass, with no AddressSanitizer report.

Run: `git status --short`
Expected: only the two documentation files are modified; `settings.json` shows nowhere (it is gitignored), and nothing under `models/`, `dotnet/`, `.superpowers/` or `docs/references/` is staged.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md
git commit -m "Document the narration review, compact transcript and scene retrieval" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| Review call after narrate when `narration_check` or `compact_transcript` is on; none when both are off | 4 |
| Standalone review prompt: `REVIEW_RULES`; input, outcome rendered like the narrate message, narration, `REVIEW_INSTRUCTION` | 3 (`ReviewPrompt`, `RenderOutcome`), 4 |
| `BuildReviewGrammar(bool check, bool digest)`; `consistent`, `problems` (<= 3 x 200 chars), `digest` (<= 300 chars) | 3 |
| `ParseReviewOutput` -> `Review { consistent, problems, digest, error }`; unparseable = model mistake | 3, 4 |
| Renarrate once: `onNarrationRestarted` -> `NarrationRestarted`; Play window clears; `morpheus_play` line | 4, 5 |
| Renarrate prompt: correction paragraph before `NARRATE_INSTRUCTION`, within `narrate_allowance`; seed `CallKind::Renarrate`; `narrate_thinking_tokens`; streams | 3, 4 |
| Second review seeded `attempt + REVIEW_RETRY_OFFSET`; `PlayTurn` requires `attempt < REVIEW_RETRY_OFFSET` | 4 |
| Commit the second narration and digest whatever the verdict; no loops | 4 |
| `CallKind::Review` / `Renarrate` appended after `Summarize`; seed tests | 1 |
| Cancel during review or renarrate commits nothing | 4 |
| Settings `narration_check`, `budget.review_max_tokens`, `budget.review_thinking_tokens` (0 or >= `MIN_THINKING_TOKENS`) required | 1 |
| `settings.example.json`, this machine's `settings.json`, `SettingsEdit`, Settings window (check, review thinking, read-only `review_max_tokens`) | 1, 9 |
| Telemetry `review` (one or two entries, with `consistent`, `problems`, `digest`, `error`), `renarrate` with `replaced` | 4 |
| Turn log shows verdicts, problems, digests and the replaced narration | 9 |
| `TurnRecord::digest`, `digest TEXT NOT NULL`, `SCHEMA_VERSION` 2, version-1 refusal naming file and both versions | 2 |
| `RenderTranscript(turns, proseTurns)`: newest prose, older digests, empty digest falls back to narration | 6 |
| Session passes `proseTurns` (compact on) or `INT_MAX` (off); folding always `INT_MAX` | 6 |
| `ENGINE_RULES` sentence about older turns | 6 |
| Settings `compact_transcript`, `prose_turns` >= 1; Settings window edits both | 1, 9 |
| Prefix stability, PromptBuilder token drop, folding sees full narration | 6 |
| world.json `scene` block, `SceneDecl`, loader `DataError`s naming `world.json`; sandcrawler declares it, mini does not | 7 |
| `SceneOf(world, state)`: path values, present entities with id and other string fields, trim/lower-case match, dedup | 8 |
| Query: input, scene joined by `"; "`, previous digest, newline-joined, cut to `EMBED_TEXT_CHARS`; lookups unchanged | 8 |
| `Retrieve` takes the parts; `NAME_BOOST` once for input or scene; digest no boost; `reasons`; ranking and trims unchanged | 8 |
| Telemetry `retrieval[].reasons`, top-level `scene`; Memory window shows reasons and scene | 8, 9 |
| `CallKindOf` recognises the review grammar; fixtures gain `review` replies by hand and record the review settings | 5 |
| Headless UI: Turn log with a renarrated turn, Memory window with scene reasons | 9 |
| Architecture spec and README updates; CLAUDE.md unchanged | 10 |
