# The review of a hosted turn (sub-project G3) - Design

Date: 2026-11-16. Source: requirement R9 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` ("Review/renarrate (your Review pass) available per turn as an option, off by default: Mantella streams speech, so a review means buffering the whole line (~1-2 s); we want to measure it before enabling it"). The owner asked to finish the remaining sub-projects in one last pass and left the design calls to the controller; they are recorded below. Last of G's three parts (G1 `2026-11-16-decide-speaker-design.md`, G2 `2026-11-16-generation-controls-design.md`).

## Goal

A host that wants a check on a hosted turn's spoken line asks for it per turn. The engine then holds the line back, has the model review it against the turn's requested action and the speaking rules, speaks it once more when the review finds problems, and only then streams the line it commits. Off by default, so a turn that does not ask is exactly as before.

## Decisions (controller, on the owner's instruction)

- Per turn: the request's optional `review` (a boolean, default false). No settings key and no host-data key: the host decides turn by turn (for example only for important NPCs) and can measure the cost.
- What the review checks: the line is only spoken words (no name or label in front, no action in brackets, no narration or stage directions, no quotation marks around it); it agrees with the speaker's requested action (it does not promise or describe another action, and does not present a refused action as done); it does not state outcomes the host has not reported. These are the faults the live runs showed (a line that began `Hulda [wait]: ...`).
- One correction at most, as Morpheus's own narration review does: a line the review finds wrong is spoken once more with the problems, and the second line is committed whatever it says.
- The review's system text is part of the prompt pack (key `review`), so a host can tune what counts as a problem.

## 1. The request

- `HostedTurnRequest` gains `bool review = false`. The server's turn body takes an optional `review` (a boolean; anything else is a 400 `review: expected true or false`).

## 2. The turn with a review

After the speak call (section 2 of the hosted-world spec), when `review` is set:

1. The speak call's line is not streamed: `onToken` is not called while it is generated.
2. The review call (tag `review`): a standalone prompt.
   - System message: the run's pack `review` text (default `HOSTED_REVIEW_RULES`, below).
   - One user message: `# Speaker\n<name>\n\n`, then `# Player\n<player line>\n\n` when the turn had player text, then `# Requested action\n<id>\n\n` (with ` <params as compact JSON>` after the id when the action has parameters; `none (the proposal was refused)` for a refused one), then `# Observation\n<compact JSON>\n\n# Line\n<line>\n\n# Task\n<HOSTED_REVIEW_INSTRUCTION>`.
   - Grammar `BuildReviewGrammar(true, false)` (a boolean `consistent` and the `problems`), read with `ParseReviewOutput(text, true, false)`; `maxTokens` `review_max_tokens`, thinking `review_thinking_tokens`, the resolve call's sampling with the seed `SamplingSeed(seed, turn, CallKind::Review, 0)`, the conversation's slot. The prompt shares no prefix with the conversation's prompt, so it replaces that slot's cached prompt: the respeak and the conversation's next call re-read the whole conversation prompt.
3. When the review says the line is consistent, or its output cannot be read (a model mistake: the line stands, and the error is kept), the first line is the turn's line.
4. When it says it is not: the respeak call (tag `respeak`): the speak prompt with its last user message replaced by `HOSTED_RESPEAK_CORRECTION`, one `- <problem>` line per problem, a blank line, then the speak instruction. The message must fit `narrate_allowance`: problems are dropped from the end until it fits; when no problem fits (all were dropped), the respeak uses the plain speak instruction, as a correction that lists no problem says nothing (the turn is still corrected by one more sampling). Sampling: the narrate settings, seed `SamplingSeed(seed, turn, CallKind::Renarrate, 0)`. Its line is the turn's line.
5. The turn's line is then delivered to `onToken` in one piece, so the host's stream carries it as tokens and sentences as before, after the review.

The cancel token is checked after each call; a cancelled turn commits nothing.

## 3. The record

- `timings.review`: the review call's telemetry, with `consistent` (a boolean, absent when the output could not be read), `problems` (an array) and `error` (a string, only when the output could not be read).
- `timings.respeak`: the respeak call's telemetry, when one ran.
- The committed `line` is the delivered one. No store change.

## 4. Texts

- `HOSTED_REVIEW_RULES` (`EngineText.h`, the pack's default `review`, no variable):

```
# Review
You check one spoken line of a character in a role-playing game that a host game runs. Reply with one JSON object and nothing else: "consistent" is false when the line breaks a rule below, true otherwise, and "problems" names each broken rule in one short sentence, at most three (empty when consistent).
The rules:
- The line is only what the speaker says aloud: no name or label in front, no action in brackets, no narration or description of actions, no quotation marks around it.
- The line agrees with the action the speaker requested: it does not promise or describe a different action, and it does not present a refused action as done.
- The line does not state an outcome the host has not reported.
Tone, style and details the rules do not decide are not problems.
```

- `HOSTED_REVIEW_INSTRUCTION`: `Review the line above against the rules. Reply with the JSON object only.` (engine framing, like the section headings).
- `HOSTED_RESPEAK_CORRECTION`: `Your previous line broke the speaking rules. Say it again without these problems:` (engine framing).
- The pack gains `review` (no variable), listed after `condense` in `PROMPT_TEXTS`; the defaults route lists it. The check that the system text fits `engine_rules_cap` does not apply to it (the review prompt is standalone and its budgets are `review_*`).

## 5. Server

- The turn route reads `review`. With it, the `action` event comes as before, then the line's `token` and `sentence` events after the review (and the respeak), then `done`.
- `done`'s `timings` carry `review` and `respeak` when present (as `decide`).
- API 1.5.0 (additive).

## Errors

- A malformed `review`: 400 at the server.
- An unreadable review output: kept in `timings.review.error`; the turn goes on with the first line.
- Everything else is the existing turn's.

## Tests (written first)

- `[hosted][review]`, fake backend: without `review` nothing changes (two calls, the line streams during the speak call); with it and a consistent review, three calls (resolve, speak, review), no token before the review, the line delivered once after it, `timings.review.consistent` true; with an inconsistent review, a fourth call (respeak) whose last message holds the correction and the problems, the second line committed and delivered, the first never delivered, `timings.respeak` present; an unreadable review: the first line kept, `timings.review.error` set; the review call's spec (grammar, tag, `maxTokens`) and prompt (each section, `none (the proposal was refused)` for a refused action); a respeak message over `narrate_allowance` drops problems; a host pack's `review` text is the review call's system message; a cancel during the review commits nothing.
- `[hosted-server]`: `review: true` streams the action, then the tokens after the review call, then `done` with `timings.review`; `review: "yes"` is a 400.
- Live (`[server]`): one turn of the conversation fixture with `review: true` completes and commits (only what cannot depend on the model's words is checked).

## Out of scope

- A review on every turn by default, or a settings or host-data switch.
- A second correction round.
- A review that keeps the conversation's prompt cache (a continuation of the speak prompt, or a slot of its own): a later decision.

## Documentation

- README (`morpheus_server`: the turn's `review`, what it checks, its cost, the pack's `review` text), CHANGELOG (API 1.5.0), the server spec's turn route, the prompt-pack spec's table (the eighth text).
