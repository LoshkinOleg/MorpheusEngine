# Global constraints (the final pass: G3 and the follow-ups)

Verbatim from docs/superpowers/plans/2026-11-16-final-pass.md.


- The request's `review`: `HostedTurnRequest::review` (`bool`, default false); server body `review` optional boolean, else 400 `review: expected true or false`.
- Review call: tag `review`; standalone prompt: system = the pack's `review` text; user = `# Speaker\n<name>\n\n` + (`# Player\n<player line>\n\n` when there was player text) + `# Requested action\n<id>` (+ ` <params compact JSON>` when non-empty; `none (the proposal was refused)` for a refused one) + `\n\n# Observation\n<compact JSON>\n\n# Line\n<line>\n\n# Task\n` + `HOSTED_REVIEW_INSTRUCTION`; `stableCount` 1. Grammar `BuildReviewGrammar(true, false)`, read by `ParseReviewOutput(text, true, false)`; `maxTokens` `review_max_tokens`; thinking `review_thinking_tokens`; sampling `SamplingFrom(resolveSampling, SamplingSeed(seed, turn, CallKind::Review, 0))`.
- Respeak call: tag `respeak`; the speak prompt with the last user message = `HOSTED_RESPEAK_CORRECTION` + `\n` + one `- <problem>\n` per problem + `\n` + the speak message; problems dropped from the end until it fits `narrate_allowance`; if it still does not fit, the plain speak message. Sampling `SamplingFrom(narrateSampling, SamplingSeed(seed, turn, CallKind::Renarrate, 0))`; same `maxTokens`/thinking as the speak call.
- With `review`: no `onToken` during the speak or respeak call; the committed line goes to `onToken` once, after the review (and the respeak). An unreadable review keeps the first line.
- `timings.review` = CallTelemetry + `consistent` (when read) + `problems` + `error` (when unreadable); `timings.respeak` = CallTelemetry when it ran. `done` carries both when present.
- Texts, exactly: `HOSTED_REVIEW_RULES` (spec section 4's block, verbatim); `HOSTED_REVIEW_INSTRUCTION` = `Review the line above against the rules. Reply with the JSON object only.`; `HOSTED_RESPEAK_CORRECTION` = `Your previous line broke the speaking rules. Say it again without these problems:`.
- The pack's eighth text: key `review`, no variable, after `condense` in `PROMPT_TEXTS`; the unknown-text list becomes `system, resolve, speak, turn_cue, decide, summarize, condense, review`.
- API version `1.5.0`.
- E2: the run's latest seen clock is the highest clock among visible records, raised to the game time of the most recent rollback that is not undone (open or abandoned) when that is higher. A rewind that changes nothing (`rewound` false) does not resync the index.
- Morpheus's own game unchanged: `"[turn]"`, `"[replay]"`, `"[context]"` stay green.
- CLAUDE.md rules: fail fast; comments that read top to bottom; a private helper only with more than one caller; constants ALL_CAPS; ASCII only; tests first; no legacy paths.
- Catch2 cannot stringify `std::string_view` here: wrap it in `std::string(...)` inside a CHECK.
- After changing a struct used across files (`HostedTurnRequest`, `PromptPack`), build once with `cmake --build --preset tests --clean-first`; if a test you did not touch crashes (SIGSEGV or a "Debug Assertion Failed" dialog), clean-rebuild before changing code.
- Commit trailer `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`.

Commands (Git Bash, repo root): `cmake --preset tests` after adding a file; `cmake --build --preset tests`; one tag `./build/tests/bin/Debug/morpheus_tests.exe "[review]"`; suite `ctest --preset tests`. Foreground only, one at a time, 600000 ms timeout; leave nothing running.
