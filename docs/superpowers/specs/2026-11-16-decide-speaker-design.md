# The engine chooses the speaker (sub-project G1) - Design

Date: 2026-11-16. Source: requirement R8 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` ("`turn` accepts ... `speaker: \"decide\"` (the engine picks the next speaker among participants, constrained by grammar)"; accept: "a fixture with two NPCs talking for 6 turns with no player input produces alternating, valid turns"), with the owner's decisions. Sub-project G was split into G1 (this), G2 (generation controls: sampling fields, call tag, thinking formats) and G3 (the optional review of a hosted turn). It builds on D1's hosted runs, D2's `morpheus_server`, E1's per-speaker memory and F's prompt pack (`2026-11-16-prompt-pack-design.md`).

## Goal

A host that does not know who should talk next sends `speaker: "decide"`, and the engine chooses one of the conversation's NPCs, then plays that NPC's turn exactly as if the host had named it.

## Owner decisions

- The choice is a model call under a grammar whose only values are the candidates' ids, made before the speaker's own resolve call.
- The last speaker is left out of the choice when the turn has no player text and no new events, recorded since the conversation's last turn or sent with the request (NPC-to-NPC chatter alternates), as long as another candidate remains. With player text or new events, any candidate may answer, the last speaker included.
- The decide call sees a neutral view of the conversation (every line, whoever witnessed it), not an NPC's own view; it writes only an id.

## 1. The request

- `HostedTurnRequest::speakerId` may be `"decide"` (`turn::DECIDE_SPEAKER`). The server's 400 for it goes.
- `"decide"` is reserved: `HostedWorld::Observe` refuses an actor whose id is `decide` ("observation at /actors/<i>/id: 'decide' is reserved: it asks the engine to choose the speaker"), collected with Observe's other faults.

## 2. The candidates

- The conversation's participants that are in the turn's observation and are not the player, in join order.
- "No input": after cleaning (as `RunTurn` cleans it today) the turn has no player text, and no new events (recorded since the conversation's last turn or sent with the request). On a turn with no input, the speaker of the conversation's last visible turn is left out when at least one other candidate remains.
- No candidate: a `DataError` "speaker: \"decide\" needs an NPC participant in the observation", before any model call or record.
- One candidate: it is the speaker; no model call is made.

## 3. The decide call

- The prompt, assembled as a hosted prompt is (`context::AssembleHostedPrompt`, so the budgets are the resolve prompt's):
  - the system message every speaker of the conversation gets: the run's pack's system text, the host's always-on lore and the participants (so it is cached with theirs);
  - L3: every visible turn of the conversation, whoever witnessed it (the entries `SyncHistory` renders, witness `""`), from the conversation's `folded_through` on; when they are over the transcript budget, the oldest are left out until they fit (no fold: the decide view is no NPC's memory); when the tail is over `tail_allowance`, the oldest new events leave it as they leave a resolve tail (the store keeps them); when it still does not fit, a `TurnError` before any record names the tail ("the decide prompt does not fit: its tail uses N tokens, over tail_allowance M") or the context ("... does not fit the context ...");
  - no memories and no retrieval;
  - the tail: the observation, the new events and the player's line as a resolve tail shows them, then `# Who may speak` with one line `- <name> (<id>)` per candidate in candidate order, then the instruction (the pack's `decide` text, `{player}` filled).
- The grammar: `grammar::FromSchema` of `{"type": "object", "properties": {"speaker": {"enum": [<candidate ids>]}}, "required": ["speaker"], "additionalProperties": false}`.
- The spec: `maxTokens` = the token count of `{"speaker":"<longest candidate id>"}` plus 8; the resolve call's sampling; no thinking; the conversation's slot (as the turn's other calls).
- The answer is parsed and validated against the schema. A valid one names the speaker. Any other (a parse failure, a validation error, an answer cut at `maxTokens`) is an LLM mistake, not an exception: the turn goes on with the candidate who spoke least recently in the conversation (one who never spoke counts as least recent; ties go to candidate order), and the reasons are kept.
- `onSpeaker` (section 5) is called with the speaker, then the turn runs as for an explicit speaker of that id.

## 4. The record

- The turn's `speakerId` is the chosen NPC. Its `timings` gain `"decide"`: the call's telemetry (as `"resolve"` has), with `"refused"` (the reasons) when the answer was refused. A turn whose speaker was the one candidate, or was named by the host, has no `"decide"`.
- No store change.

## 5. Components

- `turn::PromptPack` gains a seventh text, `decide`, taking `{player}`; `PROMPT_TEXTS` lists it after `turn_cue`. Its default (`EngineText::HOSTED_DECIDE_INSTRUCTION`): `Choose who speaks next: the character listed above who would most naturally speak now. Reply with the JSON object only.`
- `turn::HostedCallbacks` gains `std::function<void(const std::string&)> onSpeaker`, called once per turn with the speaker's id before the resolve call, for an explicit speaker too.
- `turn::HostedSession::RunTurn`: with `"decide"`, sections 2-4 before the existing steps. The cancel token is checked after the decide call as after the resolve call (a cancelled turn commits nothing).
- `server::Api`, the turn route: no 400 for `"decide"`; the `action` event's `speaker` is the id `onSpeaker` reported. API 1.4.0 (additive): `speaker: "decide"`, the pack's `decide` text, the defaults route listing it.

## Errors

- No candidate, or a reserved `decide` actor id: the host's `DataError` (400).
- A refused decide answer: logged in `timings.decide.refused`; the turn goes on (section 3).
- Everything else is the existing turn's.

## Tests (written first)

- `[hosted][decide]`, fake backend:
  - candidates: the player and NPCs absent from the observation are never candidates; with no input the last speaker is left out, with player text or with events it is not; when leaving it out would leave no one, it stays; no candidate is the DataError and nothing is recorded or called;
  - one candidate: no decide call (the backend sees only resolve and speak), no `timings.decide`;
  - the decide prompt: its system message equals the chosen speaker's resolve prompt's; L3 holds a turn that only another NPC witnessed; no `# What ... remembers`; the tail ends with the candidate list and the pack's `decide` text; the grammar's enum is the candidate ids;
  - a host pack's `decide` text replaces the default and fills `{player}`; an unknown variable in it fails the open;
  - a refused answer (an answer the schema refuses, given by a fake that ignores grammars) goes to the least recent speaker and keeps the reasons;
  - `onSpeaker` is called once, before `onAction`, for decided and explicit speakers;
  - a cancel after the decide call commits nothing;
  - an observation actor with id `decide` is a DataError naming its pointer.
- `[hosted-server]`: a decided turn's `action` event names the chosen speaker; the defaults route lists `decide`.
- A hosted fixture `decide.json`: Hulda and Saadia with Aela (no Belethor); turns with `"speaker": "decide"` and scripted decide answers, with and without player text; each step expects its speaker. It runs in-process, over HTTP and on the real model; on the real model only validity is checked (the speaker is a candidate, and never the last speaker on a turn with no input).
- The radiant fixture (two NPCs, six turns with no player input) gains a variant with `"decide"` on every turn: its first turn has no turn before it, so both NPCs are candidates and one decide call runs; after that, with two NPCs and no input there is one candidate, so it alternates with no decide call.

## Out of scope

- A separate `speaker` event in the stream (the `action` event carries the speaker).
- G2: call tags (the decide call gets one then), sampling fields, thinking formats. G3: the review.

## Documentation

- `README.md` (`morpheus_server`: `speaker: "decide"`, the pack's `decide` text), `CHANGELOG.md` (API 1.4.0), the server spec's turn route, the prompt pack spec's table (the seventh text, pointing here), `tests/hosted/fixtures/README.md` (`decide.json`, a step's `"speaker": "decide"` and its scripted decide answer).
