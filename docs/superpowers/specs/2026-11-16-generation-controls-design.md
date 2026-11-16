# Generation controls (sub-project G2) - Design

Date: 2026-11-16. Source: requirement R9 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` ("Sampling fields: min_p, repeat/presence penalty or DRY, stop strings; an explicit `callTag` in `GenerationSpec` so `CallKindOf` (Replay.h:27) no longer parses grammar text"; "Thinking formats as data (`ThinkingFormat {open, close, closeNudge}`), presets for Qwen and \"none\"; thinking OFF by default for dialogue (latency)"), with the owner's decisions. Second of G's three parts (G1 the decide speaker, `2026-11-16-decide-speaker-design.md`; G3 the optional review of a hosted turn).

## Goal

Three small controls over model calls: one more sampling field, an explicit tag on every call the turn layer makes, and the thinking block's markers as data instead of hard-coded Qwen texts.

## Owner decisions

- Sampling gains `min_p` only (no DRY, no repetition penalties, no stop strings).
- Thinking formats: presets (`auto`, `qwen`, `none`) and a custom format given as its texts.
- The call tag is a string on `GenerationSpec`.
- Thinking stays opt-in through the budgets; the README recommends 0 for hosted dialogue.

## 1. min_p

- settings.json: each `sampling` entry (`resolve`, `narrate`) takes an optional `min_p`, a number in [0, 1); absent means 0 (no min_p), so existing settings files load as they did. Outside the range: `DataError` "<file>: sampling '<call>': 'min_p' must be at least 0 and below 1, not <value>". `SettingsToJson` writes it.
- `core::SamplingSettings` and `inference::Sampling` gain `minP` (default 0); `turn::SamplingFrom` copies it.
- `LlamaCppBackend`: the sampler chain is top-k, top-p, min-p (`llama_sampler_init_min_p(minP, 1)`), temperature, then the draw; greedy decoding (temperature <= 0) stays greedy.
- `LlamaServerBackend`: `samplers` becomes `["top_k", "top_p", "min_p", "temperature"]` and `min_p` carries the value (today it always sends 0).

## 2. The call tag

- `inference::GenerationSpec` gains `std::string callTag = {}`: what the call is, for the layers that tell calls apart (the replay, the app's labels). The backends ignore it.
- The turn layer sets it on every call:
  - Morpheus's own game (`Session`): `lookup`, `resolve`, `narrate`, `renarrate`, `review`;
  - hosted runs (`HostedSession`): `decide`, `resolve`, `speak`, and for a turn with a review `review` and `respeak` (G3);
  - the shared memory upkeep (`MemoryUpkeep`): `summarize` (a fold) and `condense`.
- `turn::CallKindOf(spec)` (the prompt argument goes) reads the tag: `lookup` -> Lookup, `resolve` -> Resolve, `narrate` and `renarrate` -> Narrate (a renarrate is narration, as today), `review` -> Review, `summarize` and `condense` -> Summarize. Any other tag, the empty one included, is a `ContractError` naming it: those contexts (the replay and the app) only see the owned game's calls.
- The replay's fixture spelling does not change, so the recorded fixtures stay valid: a renarrate's reply is still taken from the `narrate` replies in call order, and a condense's from the `summarize` replies. `CallKindName` is unchanged.
- The app's `ModelSeat` labels a call from `CallKindOf` as today; its current special cases that tell a renarrate or a condense from the prompt's text read the tag instead.
- The rule that the lookup and review grammars start with a fixed root line, which only `CallKindOf` needed, is dropped from their comments (the grammars themselves need not change).

## 3. Thinking formats

- `core::ThinkingFormat`: five strings, as a thinking model's chat template writes its block:
  - `open`: what a prompt ends with when the call may think;
  - `skip`: what a prompt ends with when the call does not think (may be empty: the model does not think unless the block is opened);
  - `end`: the end marker the model writes;
  - `afterEnd`: what follows the end marker before the answer;
  - `close`: what the backend writes to close a block that reached its budget (a nudge to answer, then the end marker and `afterEnd`).
- `core::QWEN_THINKING` holds today's texts (`"<think>\n"`, `"<think>\n\n</think>\n\n"`, `"</think>"`, `"\n\n"`, and the close sentence). The `THINKING_*` constants in `src/inference/Llm.h` go; their users take the format instead.
- `core::ThinkingSetting`: a mode (`Auto`, `Format`, `None`) and the format (used by `Format`).
- settings.json `thinking_format` (optional; absent is `"auto"`):
  - `"auto"`: today's behaviour: the Qwen format when the chat template has the `enable_thinking` switch, none otherwise;
  - `"qwen"`: the Qwen format, whatever the template says;
  - `"none"`: the model has no thinking block, so the backends write none. It is not a way to switch a thinking model's thinking off: that is `"auto"` (or `"qwen"`) with every thinking budget at 0, and then the backend writes the empty block;
  - an object `{"open", "skip", "end", "after_end", "close"}`, all strings, all required.
- Checks when settings load (`DataError` naming the key): an unknown preset name; a custom format whose `open` or `end` is empty, or whose `close` does not end with `end` followed by `after_end`; an unknown or missing key of the object; with `"none"`, a thinking budget above 0 ("thinking_format is \"none\": budget '<key>' must be 0").
- The backends' configs (`llama::LlamaConfig`, `llamaserver::LlamaServerConfig`) gain `core::ThinkingSetting thinking`; the app, `morpheus_server`, `morpheus_probe` and `morpheus_play` pass the one settings.json names. Each backend resolves it when it loads (`Auto` reads the template as today) and tokenizes the format's texts as it tokenizes today's. The in-process backend requires `end` to be a single token (it recognises the end of the block by that token while generating), with an InferenceError naming the marker when it is not; the other texts may be any tokens.
- A call with a thinking budget when the resolved format is none stays today's InferenceError ("... has no thinking mode ...").

## Errors

- settings.json: the `DataError`s above.
- A missing or foreign call tag where `CallKindOf` reads it: `ContractError` (an engine bug).
- A thinking format whose markers the model cannot use: the backend's `InferenceError` at load.

## Tests (written first)

- Settings (`[settings]`): `min_p` absent is 0, a value round-trips, out of range is the DataError; `thinking_format` absent is auto, each preset, a custom object round-trips, each fault of the object, `"none"` with a non-zero budget.
- Call tags: every call of an owned turn (lookup, resolve, narrate, renarrate, review), of a hosted turn (decide, resolve, speak) and of the memory upkeep (summarize, condense) carries its tag (the fake backends record the spec); `CallKindOf` maps each owned tag and throws for an empty or hosted one; the replay of every recorded fixture under `tests/data/replays` still passes.
- `LlamaServerBackend` against the fake server: the request's `samplers` and `min_p`; with a custom format, the prompt ends with its `skip` or `open` and the thinking request stops at its `end`.
- `ModelSeat`: labels from tags, a renarrate and a condense included.
- Live (with the models): the `[llama]` and `[server]` suites with `min_p` set in the test settings; the thinking tests with an explicit `qwen` setting as well as `auto`.

## Out of scope

- DRY, repetition and presence penalties, stop strings.
- A host choosing sampling per run.
- G3: the optional review of a hosted turn.

## Documentation

- `README.md`: `min_p` in the settings section, `thinking_format` and its forms, and in the `morpheus_server` section the recommendation to set the resolve and narrate thinking budgets to 0 for hosted dialogue (latency).
- `settings.example.json`: `min_p` in both sampling entries (0.0) and `"thinking_format": "auto"`.
- The architecture spec's backend section: the thinking format is settings data.
