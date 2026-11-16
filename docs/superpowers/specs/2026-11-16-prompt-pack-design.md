# The host's prompt pack (sub-project F) - Design

Date: 2026-11-15. Source: requirement R7 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` ("Everything that is text today ... can be supplied by the host per run as data, with Morpheus's current texts as the default pack"; "Template variables are named and validated at load (unknown variable = error at `open`, not at turn time)"; accept: "byte-identical prompt prefix across turns of one conversation up to the volatile layer", "pack validation errors name the template and variable"), with the owner's decisions. It builds on D1's hosted runs (`2026-11-16-hosted-world-design.md`), D2's `morpheus_server`, E1's per-speaker memory and E2's rollback.

## Goal

The host replaces the texts a hosted run speaks to the model with its own, per run, as data. A text it does not send stays the engine's default. A mistake in the pack fails the `open` that sent it, naming the text and the variable, never a turn later.

## Owner decisions

- The pack covers the six hosted texts: the system text, the resolve instruction, the speak instruction, the turn cue, and the summarize and condense instructions. The section headings (`# Lore`, `# Participants`, `# What <name> remembers`, `# Relevant`, `# Observation`, `# New events`, `# Player`, `# Task`) and the line formats (`<name> [<action> <params>]: <line>`, `(<event>)`, `(told by <teller>)`) stay the engine's.
- F also improves two default texts that the D1 review found weak on Qwen3-4B: the speak instruction stops inviting the model to name the player, and the system text says most turns need no action.
- The pack arrives with the host data at `open`, so it is stored and hashed with it; there is no route that changes it apart from `open`.
- `GET /v1/prompts/default` gives the host the default texts and the variables each one takes.
- Morpheus's own game (owned runs, `EngineText`'s owned texts) is not touched.

## 1. The pack

The open body (and `turn::HostData`) gains an optional `prompts` object:

| Key | The text | Where it goes | Its variables |
|---|---|---|---|
| `system` | the system text | L0, the start of every hosted prompt's system message | none |
| `resolve` | the resolve instruction | the `# Task` section of a turn's tail | `{speaker}`, `{player}` |
| `speak` | the speak instruction | the user message the speak call appends | `{speaker}`, `{player}` |
| `turn_cue` | the turn cue | L3's user message for a turn with no events and no player text before the line | `{speaker}`, `{player}` |
| `decide` | the decide instruction | the decide call's tail, after the `# Who may speak` list (added by G1, `2026-11-16-decide-speaker-design.md`) | `{player}` |
| `summarize` | the summarize instruction | the end of an NPC's fold call | `{actor}` |
| `condense` | the condense instruction | the end of an NPC's condense call | `{actor}` |

- The host may send any subset; a key it leaves out takes the engine's default (`EngineText`'s `HOSTED_*` constants). An empty `prompts` object is the same as none.
- `HostDataFromJson` checks the shape: `prompts`, when present, is an object whose every value is a string (a `DataError` "host data at /prompts...: ..." otherwise). `HostData::prompts` holds what the host sent, key by key.
- The content is checked when the run's `HostedWorld` is built (section 2): an unknown key is a `DataError` "prompts/<key>: unknown text; the pack's texts are system, resolve, speak, turn_cue, decide, summarize, condense"; an empty string is a `DataError` "prompts/<key>: empty".
- `HostDataToJson` writes `prompts` only when the host sent at least one text, and then only the texts it sent. A run that sent no pack keeps the content hash it had before F, so existing runs reopen as they are, and a run follows changes to the engine's defaults for every text it did not send.
- A changed pack is changed host data: `open` takes D1's existing re-embed-then-replace path. The next turn renders its whole prompt, conversation entries included, with the new texts.

## 2. Variables

- A variable is `{` followed by one or more of `a-z` and `_`, then `}`. Every other brace is literal text, so a JSON example such as `{"action": "wait"}` in the system text stays as written.
- Each text takes only the variables in section 1's table. A variable a text does not take is a `DataError` at open: "prompts/<key>: unknown variable {<name>}; this text takes {speaker}, {player}" (or "takes no variable" for `system`, or "takes {actor}"). The first fault found is reported: unknown keys first (in key order), then each text in the table's order (empty, then its variables in text order).
- No variable is required: a text may name no one.
- The system text takes no variable so that L0 is the same for every turn and speaker of a run (R7's cache rule).
- The engine fills variables with names: `{speaker}` the speaking NPC's, `{player}` the player's character's, `{actor}` the NPC whose memory is folded or condensed. One filler, `PromptPack::Fill`, replaces today's `FillVariables` and the `{actor}` loop in `HostedSession`.

## 3. Checks against the budget

- The system text (the host's or the default) must fit `engine_rules_cap`. `HostedSession::Open` counts it with the run's backend after the host data and settings checks and before it creates or touches the file; over the cap is a `DataError` "prompts/system: <n> tokens, over engine_rules_cap <cap>". `AssembleHostedPrompt` keeps its `MORPHEUS_REQUIRE`, which can then fail only through an engine bug.
- The speak instruction must fit `narrate_allowance` once filled. Names change per turn, so this stays the per-turn `DataError` `RunTurn` raises, now starting "prompts/speak: the speak instruction for '<speaker>' uses <n> tokens, over narrate_allowance <cap>".
- Not checked at open (the names they hold are only known at a turn):
  - `resolve` sits in the tail and counts against `tail_allowance`; a turn over it is the `TurnError` "the observation, the events, the player text and the resolve instruction (prompts/resolve) use <n> tokens, over tail_allowance <cap>", which names `prompts/resolve`.
  - `speak`: the per-turn `DataError` above.
  - `turn_cue` counts in L3's entries: it counts in the transcript, which folds.
  - `summarize` and `condense` are not budget-checked; an oversize text makes the fold or the condense take the engine's existing fallback summary, as any over-long memory call does.

## 4. Components

- `turn::PromptPack` (new, `src/turn/PromptPack.{h,cpp}`): the seven texts as members (`decide` added by G1), defaulted to `EngineText`'s `HOSTED_*` constants; `PromptPack::FromHost(const std::map<std::string, std::string>&)`, which applies the host's texts over the defaults and checks them (sections 1 and 2); `Fill(text, variables)`; and the table of each text's variables, which the check, `Fill` and the defaults route share.
- `turn::HostData` gains `prompts` (the host's texts, by key); `HostDataFromJson` and `HostDataToJson` read and write it (section 1).
- `turn::HostedWorld` builds the run's `PromptPack` in its constructor, with the other host data checks, and exposes it (`Prompts()`).
- `turn::HostedSession` reads every hosted text from `_world.Prompts()` instead of the constants: the system text in `Pass`, the resolve and speak instructions and the cue in `RunTurn` and `Entries`, the summarize and condense instructions in its upkeep scope. `Open` adds section 3's check.
- `server::Api`: `open` passes `prompts` through `HostDataFromJson`; the new `GET /v1/prompts/default` answers `{"api_version": ..., "prompts": {<key>: <default text>, ...}, "variables": {<key>: ["speaker", "player"], ...}}` (a JSON object, so its keys come in no set order; `system`'s list is empty). API 1.3.0: a new optional field and a new route; nothing breaks.

## 5. The default texts

- The speak instruction becomes: `Step 2: write what {speaker} says now. Spoken words only, in character.` ("{player} is the player's character" made the model name the player in most lines.)
- The system text's "Step 1, act." paragraph gains, after "Choose what the speaker would do now.": `Most turns need no action: when the speaker would only talk, choose the action that does nothing, if the turn allows one.` (The engine cannot name the host's no-op id; a host pack can.)
- The other four defaults stay as they are. The live `[llama][hosted]` and `[server]` tests run on the new defaults before F is done.

## Errors

- A malformed pack is the host's data: `DataError` at open, mapped by the server to 400 as every host-data fault is, naming `prompts/<key>` and, for a variable, the variable and what the text takes.
- An over-cap system text is a `DataError` at open (section 3).
- No fallback: a text that fails a check is never replaced by the default.

## Tests (written first)

- `tests/turn/PromptPackTests.cpp` (`[prompts]`): the defaults are `EngineText`'s; a partial pack keeps the other defaults; an unknown key, an empty text, an unknown variable in each text (the message names the key, the variable and what the text takes), a variable in `system`; literal braces (a JSON example) pass and stay; `Fill` fills every variable a text takes, repeated ones included, and leaves literal braces alone.
- `HostDataFromJson` and `HostDataToJson`: `prompts` of the wrong shape names its pointer; the round trip keeps only the sent texts; host data without a pack hashes as before F (a pinned hash from a host data JSON with no `prompts`).
- Hosted session with a custom pack (fake backend): the resolve prompt's system message starts with the custom system text, its tail ends with the filled custom resolve instruction, the speak call's message is the filled custom speak text, a turn with no events and no player text shows the custom cue in L3, and a fold and a condense end with the filled custom instructions.
- R7's acceptance: over three turns of one conversation with a custom pack, each resolve prompt's messages up to its `stableCount` are byte-identical to the previous prompt's messages, and only the tail and the new entries differ.
- Open: an over-cap custom system text is a `DataError` naming `prompts/system`, and no file is created; reopening with a changed pack replaces the stored host data and the next prompt shows the new text.
- Server (`[hosted-server]`): `open` with a pack, then a turn whose prompt shows it; `open` with a bad variable answers 400 naming the key and the variable; `GET /v1/prompts/default` answers every key with its default and its variables.
- A hosted fixture with a pack (`tests/hosted/fixtures/prompt_pack.json`), run in-process and over HTTP like the others.
- Existing tests that pin the old default speak instruction or system text are updated to the new defaults; `"[turn]"`, `"[replay]"` and `"[context]"` (Morpheus's own game) are unchanged.

## Out of scope

- The section headings and line formats (owner decision).
- Morpheus's own game's texts and review (`ENGINE_RULES`, `NARRATE_INSTRUCTION`, ...).
- G: `"decide"`, the optional review, an explicit call tag, sampling fields, thinking formats as data.
- E2's follow-ups (a record's clock right after a rewind; a no-op rewind's resync).

## Documentation

- `README.md`, the `morpheus_server` section: `prompts` in the open body, its keys and variables, and the defaults route.
- `CHANGELOG.md`: API 1.3.0.
- The server spec's route table and the hosted-world spec's "Default hosted texts" (pointing here).
- `tests/hosted/fixtures/README.md`: `prompts` in a fixture's host data.
