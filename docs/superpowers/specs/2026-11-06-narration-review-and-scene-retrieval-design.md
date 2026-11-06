# Narration review, compact transcript and scene retrieval - Design

Date: 2026-11-06. Source: the brief `.superpowers/briefs/2026-11-06-narration-check-and-compact-transcript.md` (MorpheusAmaAgent), with the owner's decisions from the brainstorming session that followed. One spec, one plan, one milestone.

## Goals

1. **Narration review.** After every narration, a review call checks the prose against the engine's outcome. A narration that contradicts it is narrated once more and replaced.
2. **Compact transcript.** Older turns appear in the prompt as a one or two sentence digest instead of their full narration, so the model copies its own prose less and the transcript layer grows more slowly.
3. **Scene retrieval.** Automatic retrieval also searches with the current scene (where the player is, who is there) and the previous turn's digest, not only the player's input.

## Background facts that shape the design

- **Prompt cache.** `LlamaCppBackend` reuses the longest common prefix with the last prompt it evaluated, in one sequence. Qwen3.5 and later (Qwen3.8, the recommended Qwen3.5-9B) mix linear-attention layers into the model and cannot cut their cache back to a prefix, so every call evaluates its whole prompt. Qwen3-4B-Instruct reuses the prefix.
- **Streaming.** The narration streams to the player (`TokenStreamed`) as it is written, so any check runs after the player has seen it.
- **Model mistakes are not exceptions** (CLAUDE.md): an unparseable or contradicting model output is recorded with a reason and the turn continues.

## 1. Narration review

### The review call

After narrate, the session makes one review call when `narration_check` or `compact_transcript` is on. When both are off, no call is made and the turn is as today.

The review prompt is standalone and short, not an extension of the narrate prompt:

- **System message:** `REVIEW_RULES` (new, in `EngineText.h`): the reviewer's job, what counts as a contradiction (a failed check narrated as a success or the reverse, an applied change left out or reversed, a rejected change narrated as if it happened), and what a digest is (one or two sentences, past tense, third person, who did what and what came of it; no dice or engine terms).
- **User message:** the player's input; the engine's outcome, rendered the way the narrate message renders it (checks with success or failure, applied changes, rejected changes with their reasons); the narration; then `REVIEW_INSTRUCTION`.

Why standalone: it is about 300 to 600 tokens whatever the run's length, so its cost is the same on every model, and it needs no reservation in the context fit. The review needs only the outcome and the narration; the world rules and the transcript add nothing to either job.

Cost on Qwen3-4B-Instruct: the review prompt replaces the narrate prompt in the backend's cache, so the next turn's resolve call evaluates its whole prompt instead of reusing its prefix (about a second on that model). Accepted: the recommended models reuse nothing anyway. A second cache sequence for side calls would remove this cost and is not part of this work.

### Output

A grammar (`BuildReviewGrammar(bool check, bool digest)`, in `turn/Grammar`) constrains the output to one JSON object:

- `"consistent"`: boolean. Present only when the check is on.
- `"problems"`: array of at most `REVIEW_MAX_PROBLEMS` (3) strings of at most `REVIEW_PROBLEM_MAX_CHARS` (200) characters. Present only when the check is on. Empty when consistent.
- `"digest"`: string of at most `DIGEST_MAX_CHARS` (300) characters. Present only when `compact_transcript` is on.

`ParseReviewOutput` returns a `Review { std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`. Output that does not parse (for example cut off by `review_max_tokens`) is a model mistake: `error` says why, `consistent` is empty, the digest is empty, and the turn goes on as if the review had found nothing.

### Renarrate

When the check is on and the first review says `"consistent": false`:

1. The session notifies `TurnCallbacks::onNarrationRestarted` (new), which the worker turns into a `NarrationRestarted` event. The Play window clears the turn's streamed text; `morpheus_play` prints a line `[narration rewritten: <problems>]` and streams the new text below it.
2. It narrates once more with the same narrate prompt, except that the narrate message gains a paragraph before `NARRATE_INSTRUCTION`: the previous narration contradicted the outcome, the problems, and the instruction to narrate the outcome exactly. The narrate message stays within `narrate_allowance`, cut the way it is today. Sampling seed: `CallKind::Renarrate`. Thinking budget: `narrate_thinking_tokens`. It streams like the first narration.
3. It reviews the second narration (seed `CallKind::Review`, attempt offset as below).
4. It commits the second narration and the second review's digest, whatever the second verdict says. There is at most one renarrate per turn: no loops.

`core::CallKind` gains `Review` and `Renarrate`, appended after `Summarize`, so existing seeds and fixtures keep their values. The second review's seed uses `SamplingSeed(seed, turn, CallKind::Review, attempt + REVIEW_RETRY_OFFSET)` with `REVIEW_RETRY_OFFSET = 1 << 23`, so it never collides with a regenerated turn's first review. `PlayTurn` requires `attempt < REVIEW_RETRY_OFFSET` (`MORPHEUS_REQUIRE`), which keeps the offset attempt within `SamplingSeed`'s limit of 2^24.

A cancel during the review or the renarrate cancels the turn as today (`std::nullopt`, nothing committed).

### Settings

`settings.json` gains required keys (a missing key is a `DataError` naming it, like every other key):

- `"narration_check": true` - top level.
- `"budget": { "review_max_tokens": 384, "review_thinking_tokens": 0, ... }` - the review call's answer and thinking budgets. `review_thinking_tokens` follows the same rule as the other two (0, or at least `MIN_THINKING_TOKENS`).

`settings.example.json`, this machine's `settings.json`, `SettingsEdit` and the Settings window gain them: the window edits `narration_check` and `review_thinking_tokens` (beside the other thinking budgets); `review_max_tokens` joins the read-only budget list.

### Telemetry and the Turn log

`record.timings` gains:

- `"review"`: an array of one or two entries, one per review call: the call telemetry (`Telemetry(completion, spec)`), plus `consistent`, `problems`, `digest` and `error` when set.
- `"renarrate"`: the renarrate call's telemetry and `"replaced"`: the first narration's text. Present only when a renarrate happened.

The Turn log shows each review's verdict, problems and digest, and for a renarrated turn the replaced narration. The Play window shows nothing extra (owner decision): only the clear-and-restream.

## 2. Compact transcript

### Storage

- `core::TurnRecord` gains `std::string digest = {}`: the last review's digest, empty when there was none.
- The `turns` table gains `digest TEXT NOT NULL`. `SCHEMA_VERSION` goes from 1 to 2, which also covers the `turns.attempt` column M3 added without a bump. A version-1 run refuses to open with the existing `StoreError` naming the file and both versions; there is no migration (owner decision).

### Rendering

`context::RenderTranscript(turns, proseTurns)`:

- User messages are unchanged: the input plus the mechanics line.
- For a turn among the newest `proseTurns`, the assistant message is the narration.
- For an older turn, it is the digest when the digest is non-empty, else the narration (turns played with `compact_transcript` off, or whose review failed).

The session passes `settings.proseTurns` when `compact_transcript` is on and `std::numeric_limits<int>::max()` when it is off. Folding (`turn/Folding.cpp`) always passes `std::numeric_limits<int>::max()`: summaries are written from the full narration (owner decision).

`ENGINE_RULES` gains one sentence in its transcript paragraph: older turns in the transcript show a short summary of what was narrated rather than the narration itself; narrate in full prose regardless.

### Settings

Required keys, top level:

- `"compact_transcript": true`
- `"prose_turns": 2` - at least 1 (`DataError` otherwise).

The Settings window edits both.

### Cache behavior

Each turn, the entry that crosses from the newest `prose_turns` into the older ones changes form, so on a prefix-reusing model the transcript is re-evaluated from that entry on (about `prose_turns + 1` entries). The models without prefix reuse evaluate everything anyway and gain from the shorter transcript on every call.

### Unchanged

The Play window shows full narration for every turn. History retrieval embeds and returns the narration as today.

## 3. Scene retrieval

### world.json

An optional `"scene"` object (the loader's allowed keys gain `scene`):

```json
"scene": {
  "paths": ["player.location", "crawler.location"],
  "present_at": "player.location"
}
```

`core::World` gains `std::optional<SceneDecl> scene` with `std::vector<std::string> paths` and `std::string presentAt`. The loader throws a `DataError` naming `world.json` and the offending value when:

- `scene` has keys other than `paths` and `present_at`, or `paths` is empty;
- a path is not `entity.field`, its entity is not in `initial_state.json`, or its field is not a `string` field of that entity's type;
- `present_at` is not one of `paths`.

Sandcrawler declares the block above. The mini test world does not, and behaves as today.

### The scene each turn

`SceneOf(world, state)` (new, in `turn/Retrieval`) returns the scene text as a list of strings:

1. The value at each declared path in the current state, skipping empty strings.
2. The entities present: every entity other than the one named by `present_at` whose field of the same name as `present_at`'s field (`location`) is a string equal to `present_at`'s value, compared after trimming and lower-casing. Each present entity adds its id and the values of its other `string` fields (for a sandcrawler NPC: its `role`), skipping empty strings.

Values are deduplicated (case-insensitive), keeping the first occurrence, in the order above (declared paths first, then entities by id).

### The query

The automatic retrieval query becomes, in this order, joined by newlines:

1. the player's input;
2. the scene values, joined by `"; "`, when there are any;
3. the previous turn's digest, when it is non-empty.

The whole query is cut to `EMBED_TEXT_CHARS` as today; the input comes first so it survives the cut. Assisted-mode lookup queries are unchanged.

### Scoring

`MemoryIndex::Retrieve` takes the query parts separately (input, scene values, digest) so it can tell why an entry matched:

- The base score is the cosine similarity with the embedding of the whole query.
- `NAME_BOOST` (+0.15) applies when a lore subject appears (lower-cased substring, as today) in the input. The reason is `name`.
- The same boost applies when the subject appears in a scene value, at most once per entry: an entry named in both the input and the scene gets +0.15, with reasons `name` and `scene`.
- The digest only enters the embedding; it adds no boost.
- Ranking, `RETRIEVAL_MIN_SCORE`, `RETRIEVAL_MAX_HITS` and the `retrieval_cap` trim are unchanged: scene hits compete on score (owner decision).

`RetrievalHit` gains `std::vector<std::string> reasons` (`"name"`, `"scene"`; empty for similarity alone). Retrieval stays deterministic given the embeddings.

### Telemetry and the Memory window

Each `"retrieval"` telemetry entry gains `"reasons"`, and a top-level `"scene"` entry records the scene values used. The Memory window shows each hit's reasons next to its score, and the turn's scene values.

### Known limit

Matching is by containment, like the name boost. A location written as "the old oasis" matches the lore subject `oasis`; "Iron Convoy depot" does not match `faction iron convoy`, whose subject is longer than the text. Such cases rely on the embedding.

## Tests (written first, Catch2)

### Review

- `BuildReviewGrammar` for each combination of check and digest; `ParseReviewOutput` on valid output, output missing fields, and cut-off output (error set, nothing else).
- Session with `ScriptedBackend`:
  - a consistent review commits the first narration and its digest, with no renarrate;
  - an inconsistent review (a failed check narrated as a success) triggers one renarrate: the second narration and the second review's digest are committed, `timings.review` holds both verdicts, `timings.renarrate.replaced` holds the first text, and `onNarrationRestarted` fired once;
  - an inconsistent second review still commits (no second renarrate);
  - an unparseable review commits the narration with an empty digest and the error in the telemetry;
  - `narration_check` off with `compact_transcript` on: the review grammar has only `digest`, and no renarrate happens whatever the narration says;
  - both off: no review call;
  - cancel during the review or the renarrate commits nothing.
- Worker: `NarrationRestarted` arrives between the two narrations' `TokenStreamed` events.
- Seeds: `SamplingSeed` for `Review` and `Renarrate` differs from the other kinds, and the existing kinds' values are unchanged.

### Compact transcript

- `RenderTranscript` with `proseTurns` 2 over four turns: turns 1-2 render digests, 3-4 narration; an older turn with an empty digest renders its narration.
- Prefix stability: the rendered messages for turns 1..N and 1..N+1 agree on every entry before turn N + 1 - `prose_turns`.
- `PromptBuilder`: the transcript layer's token count drops when digests replace narration.
- Folding still sees full narration.
- Store: the digest round-trips; a version-1 file is refused with both versions in the message.
- Settings: the new keys load; a missing key, `prose_turns` 0 and `review_thinking_tokens` 10 are `DataError`s naming the key.

### Scene retrieval

- World loading: the `scene` block loads; each invalid case above is a `DataError` naming `world.json`.
- `SceneOf`: path values; an NPC at the player's location adds its id and role; an NPC elsewhere adds nothing; case and space differences still match; duplicates collapse.
- `Retrieve`: a lore entry named by a scene value is retrieved with an input that names nothing, with reason `scene`; an entry named in both gets one boost and both reasons; scene hits are still trimmed to `retrieval_cap`.
- Session: moving the player changes which lore reaches the resolve prompt; the previous turn's digest is part of the query.

### Replays and the app

- `CallKindOf` recognises the review grammar. Replay fixtures gain a `"review"` replies list; the three existing fixtures get review replies added by hand (consistent, with digests), and their other replies stay as recorded. A fixture records the run's `narration_check`, `compact_transcript` and `prose_turns`.
- The headless UI tests draw the Turn log with a renarrated turn and the Memory window with scene reasons.

## Documentation

- Architecture spec: the turn flow gains the review step and renarrate; the context section describes digests and `prose_turns`; the memory section describes scene retrieval and the `scene` block; the events list gains `NarrationRestarted`; `CallKind` gains its two values.
- README: the new settings keys, and the `scene` block in the world format.
- CLAUDE.md: nothing changes.
