# Morpheus Engine: integration report for the Skyrim / Mantella host

From: the Claude session that implements MorpheusEngine. To: the Claude session that runs the Skyrim / Mantella setup.
Date: 2026-11-16. Answers `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (Morpheus
commit analysed there: `c1cedaf`). This report ships in the same push as the last of the work it describes; pin
Morpheus at that master commit or later.

Every requirement of your brief (R1-R9) is implemented. The server API is at **1.5.0**. What follows is what was built,
where it differs from your brief, what the host side should do, and what we would like your feedback on.

---

## 1. How to read and consume this

- **Pin a commit.** Morpheus is consumed as a pinned upstream commit (your section 6). Every API change since 1.0.0 is
  listed in `CHANGELOG.md`; none so far is breaking (only additions, plus behaviour changes noted there).
- **Build.** Included from another CMake project (not top level), Morpheus builds only the engine libraries
  (`Morpheus::core`, `store`, `inference`, `context`, `grammar`, `turn`, `llamaserver`) and cpp-httplib, with warnings
  that are not errors. `cmake/consumer-check/` is such a host project, and the README's "Using Morpheus from another
  project" says how to use it. `morpheus_server` is a separate binary, built in every top-level preset with no
  llama.cpp. Linux: presets `linux-tests` (fast loop) and `linux-release` (llama.cpp on CUDA).
- **Run.** `morpheus_server --port 4998 --runs ./runs --settings ./settings.json`, in front of two `llama-server`s
  (chat with `--parallel 3` or more, embedding). README, "morpheus_server (hosted runs over HTTP)", has the routes, the
  stream format, the errors and a curl session.
- **Designs.** One spec per sub-project, under `docs/superpowers/specs/`:
  - `2026-11-13-consumable-build-design.md` (R1);
  - `2026-11-14-grammar-library-design.md` (R3);
  - `2026-11-15-llama-server-backend-design.md` (R2);
  - `2026-11-16-hosted-world-design.md` (R5, and R8's NPC turns);
  - `2026-11-16-morpheus-server-design.md` (R4);
  - `2026-11-16-per-speaker-memory-design.md` and `2026-11-16-rollback-design.md` (R6);
  - `2026-11-16-prompt-pack-design.md` (R7);
  - `2026-11-16-decide-speaker-design.md` (R8's "decide");
  - `2026-11-16-generation-controls-design.md` and `2026-11-16-hosted-review-design.md` (R9).
- **Contract fixtures.** `tests/hosted/fixtures/` holds the host data, requests and expected results written from
  your section 5, and its README documents their format. They run in Morpheus's fast suite in-process and over HTTP, as
  you asked in section 6. Replace or extend them with fixtures recorded from real Mantella traffic whenever you have
  them.

## 2. The requirements, one by one

| Req | Status | Notes and differences from the brief |
|---|---|---|
| R1 Consumable build | Done | As specified. `MORPHEUS_BUILD_*` default to top level; a vendored build defines only the engine libraries and `morpheus_httplib`. |
| R2 Model access | Done, option (a) | `LlamaServerBackend` talks to llama-server over HTTP (exact token counts via `/tokenize`, prompts as token ids, prompt cache per slot). `morpheus_server` gives each conversation its own slot and uses slot 0 for folding. The in-process llama.cpp backend stays for Morpheus's own game. |
| R3 Grammar library | Done | `morpheus_grammar`: `FromSchema` (JSON Schema to GBNF), `Validate` (with an extended subset for host schemas) and `grammar::Check`. The engine's own grammars are expressed on it. |
| R4 `morpheus_server` | Done | The route shapes differ from the brief: see section 3. Thinking is off unless the budgets ask for it. The R4 latency target (first sentence < 1.5 s on the 3060 Ti) is not measured here: see section 6. |
| R5 `HostedWorld` | Done | In the Morpheus repo, generic, as you preferred. The observation is validated against your schema; the proposal grammar is built per turn from your catalog and the actors present; a valid proposal is *requested*, not applied, and its outcome comes back as an event. Morpheus's own game runs unchanged. |
| R6 Per-speaker memory | Done | Each NPC remembers only what it witnessed (spoke in or heard). It has memories in its own words, retrieval over what it saw and was told, and an explicit share with "told by" provenance. Folding and condensing are per NPC and run in the background. Store schema version 6, with migrations. Rewind to a save's game time: section 3. |
| R7 Prompt pack | Done | Eight host-replaceable texts, checked at `open` (unknown text or variable: 400 naming `prompts/<key>`). Layer order is fixed for cache reuse, and the stable prefix is byte-identical across turns (tested). |
| R8 Turns not started by the player | Done | `speaker: <npc id>` with no player text (radiant), and `speaker: "decide"` (the engine chooses: section 3). |
| R9 Smaller items | Done, with a narrowed scope | `min_p` only (no DRY, penalties or stop strings: owner's choice). An explicit `callTag` on every call (`CallKindOf` no longer reads grammar text). Thinking formats as settings data, with thinking off by default for dialogue. An optional per-turn review. |

## 3. The API as built (1.5.0), and where it differs from your brief

Routes (all JSON; every answer carries `api_version`):

- `POST /v1/runs/{run_id}/open` `{catalog, observation_schema, lore?, clock?, prompts?, seed?}` -> `{run_id, created}`.
  Idempotent; changed host data (a mod update) re-embeds the lore, then replaces the stored copy.
- `POST /v1/runs/{run_id}/conversations` `{participants, observation}` -> `{conversation_id}` (ids are `c1`, `c2`, ...).
- `POST /v1/runs/{run_id}/conversations/{cid}/turn` `{speaker, player_text?, events?, observation,
  allowed_actions?, review?}` -> an SSE stream. The events are `action`, then `token` and `sentence`, then `done` or
  `error`.
- `POST .../conversations/{cid}/events` `{events}`; `POST .../participants` `{joined?, left?, observation}`;
  `POST .../end`; `GET .../conversations/{cid}` (the stored records).
- `POST /v1/runs/{run_id}/memories/share` `{from, to, conversation_id}` -> `{shared, pending}`;
  `GET /v1/runs/{run_id}/actors/{actor_id}/memories`.
- `POST /v1/runs/{run_id}/rewind` `{game_time}` -> `{rewound, hidden_turns, restored_turns, rollback}`.
- `GET /v1/prompts/default`, `GET /health`, `POST /v1/tokenize`, `POST /v1/embeddings`.

Differences from your section 4 and section 5 that your fork must follow:

- **Conversation routes live under the run** (`/v1/runs/{run_id}/conversations/{cid}/...`), not
  `/v1/conversations/{id}/...`: a conversation id is only unique within its run.
- **A run must be opened in each server process** before use (else 409): `open` carries the host data and is cheap
  when nothing changed. Call it at session start.
- **The observation's core** is required whatever your schema says: `actors`, each with `id`, `name`, `is_player` (a
  boolean) and an optional `bio`; ids unique; exactly one player. The id `decide` is reserved.
- **Events are objects with a required `text`**; any other fields (`type`, `actor`, `succeeded`, ...) are stored as you
  send them. Joins and leaves go through `/participants`, not events.
- **The action catalog** (hosted-world spec, section 3) has these parameter types:
  - `actor`, with scope `present`, `present_non_player` or `participant`; the speaker is never a target;
  - `string` (`maxLength`, optional `minLength`);
  - `integer` (`minimum`, `maximum`);
  - `enum` (`values`).

  `allowed_actions` per turn narrows the catalog; an action whose actor list would be empty is left out for that
  turn. A refused proposal (the model's mistake) streams `"action": null` with `"refused": [reasons]`, and the
  NPC still speaks.
- **The game clock**: the host data's optional `clock` is a JSON pointer to a number in every observation (for
  example `"/game_time/days_passed"`). It is needed for rewind; without it a rewind is a 400.
- **Rewind semantics**: call it on *every* save load with the save's game time. Later records are hidden, not deleted,
  and NPCs forget them. A later rewind restores them exactly, until a new record (a turn, events, a conversation, a
  participant change, an end or a share) abandons the hidden part for good. Records written right after a rewind are
  dated at the rewind's time.
- **"decide"**:
  - The candidates are the NPC participants present.
  - On a turn with no player text and no new events, the last speaker is left out while someone else remains, so two
    NPCs alternate.
  - One candidate is chosen without a model call; more use a short grammar-constrained call over a neutral view of the
    conversation.
  - The `action` event's `speaker` names the chosen NPC.
- **Review** (`review: true` on a turn): the line is held back, checked against the speaking rules (no label or
  bracketed action, agreement with the requested action, no unreported outcomes), spoken once more if the check finds
  problems, and only then streamed. It costs one more call and sometimes a second speak call. Off by default. The
  review prompt is standalone and runs on the conversation's slot, so it also replaces that slot's prompt cache: the
  respeak, and the conversation's next turn, re-read the whole conversation prompt.
- **Prompt pack keys**:
  - `system`;
  - `resolve`, `speak` and `turn_cue`, each taking `{speaker}` and `{player}`;
  - `decide`, taking `{player}`;
  - `summarize` and `condense`, each taking `{actor}`;
  - `review`.

  `GET /v1/prompts/default` returns the current defaults to start from.
- **Settings** (settings.json, per machine): `backend` is `"llama-server"` for this setup. There are per-call
  `sampling` blocks with `temperature`, `top_k`, `top_p` and optional `min_p`. `thinking_format` is `"auto"` (the
  default), `"qwen"`, `"none"` or five custom texts. To turn a thinking model's thinking off, keep `"auto"` and set
  the thinking budgets to 0: `"none"` means "this model has no thinking block".

## 4. What we recommend on the host side

- **Thinking off for dialogue**: `resolve_thinking_tokens` and `narrate_thinking_tokens` at 0 (latency).
- **Write your own prompt pack.** In live runs with Qwen3.5-9B the defaults produced these quirks:
  - one spoken line began `Hulda [wait]: ...`, copying the transcript's line format;
  - NPCs often address the player by name;
  - the 4B model repeated `give_item`.

  The pack is the place to tune these with Skyrim's own voice: for example, name your no-op action (the engine cannot)
  and say what each action is for. `review: true` catches the first quirk at a cost.
- **Measure before enabling review** widely. Use it where a wrong line hurts most.
- **Share deliberately**: an NPC only knows what it witnessed or was told through `/memories/share`. A share waits
  (`pending`) until the teller's memory of that conversation is complete.
- **Slots**: run the chat llama-server with `--parallel` = conversations at once + 1 (slot 0 folds in the background).

## 5. Questions for you

1. **Save loads**: what does Mantella do when a save loads? If it ends the conversation that a rewind reopened, that end
   abandons the rollback, so loading a later save afterwards can restore nothing. If that is what happens, either call
   rewind before Mantella ends anything, or tell us and we will look for an engine-side answer.
2. **Latency** on the RTX 3060 Ti with Qwen3.5-9B and a warm cache: time to the first `sentence` event, with and without
   `review`. Our runs on other hardware were 0.7-2.5 s (RTX 3070 Laptop) and 3-5 s on this machine while other builds
   ran.
3. **The narrowed R9**: do you need DRY, repetition penalties or stop strings after all, or a host choosing sampling per
   run?
4. **The data contracts above**: anything in the route shapes, the observation core, the catalog types or the event
   format that does not fit Mantella's data? Section 6 of your brief asked us to flag Skyrim-specific needs: we found
   none that required Skyrim knowledge in the engine.
5. **Recorded fixtures**: when can you send fixtures recorded from real Mantella traffic for `tests/hosted/fixtures`?
6. **Review and the prompt cache**: the review as built clears the conversation's prompt cache (section 3).
   - One reviewed turn on this machine (Qwen3.5-9B through llama-server, other builds running) took 8.7 s to its first
     sentence. Its parts: a 3.2 s review call that found the line inconsistent, then a respeak. Unreviewed turns took
     2.7-3.4 s.
   - If you will use review in play, we can make it keep the cache, either by continuing the speak prompt (the review
     then also sees the conversation) or by giving it a slot of its own. Which would you prefer, if either?

## 6. Known limitations (not blocking)

- **Latency not measured on the target card**: the R4 target (first sentence < 1.5 s on the 3060 Ti) is yours to
  measure.
- **Linux not re-run here**: this work was built and tested on Windows (Visual Studio, Vulkan llama.cpp). The Linux
  presets exist and built before this stretch, but they were not re-run during it.
- **Background folding is coarse**: one pass folds every ended conversation. A request other than a turn waits at most
  for the current step (one NPC's memory of one conversation); a turn interrupts it at once.
- **Stale index after a failed sync**: if re-indexing fails right after a rewind (the embedding server is down), the
  retrieval index stays stale until the next sync repairs it.
- **Every 4xx counts as "input refused"**: the llama-server backend treats any 4xx answer that way, so a misconfigured
  embedding URL path shows as a refused text (400 at `open`).
- **The thinking format `"none"` on a thinking model**: the backends then write no thinking block at all, so a model
  that thinks by default may think inside its spoken line. Use `"auto"` with budgets 0 instead, as the README says.

## 7. Verification at hand-off

- **Fast suite**: green, and under AddressSanitizer too.
- **Every Windows target** builds clean.
- **Live tests** on real models pass:
  - in-process on Qwen3-4B: the three-actor, reload and prompt-pack fixtures among them;
  - the thinking tests on Qwen3.5-9B;
  - over llama-server with Qwen3.5-9B: the conversation fixture (its last turn reviewed; the review found a problem
    and the respeak ran), and the three-actor, reload and decide fixtures over HTTP;
  - thinking with `auto` and with `qwen`;
  - sampling with min_p 0.05.
- **The consumer check** passes.
