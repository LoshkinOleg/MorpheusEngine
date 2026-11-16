# Global constraints (sub-project G1, the engine chooses the speaker)

Verbatim from docs/superpowers/plans/2026-11-16-decide-speaker.md. Spec: docs/superpowers/specs/2026-11-16-decide-speaker-design.md.


- The keyword: `"decide"` (`turn::DECIDE_SPEAKER`). An observation actor with that id: the fault `observation at /actors/<i>/id: 'decide' is reserved: it asks the engine to choose the speaker`.
- Candidates: the conversation's participants that are in the turn's observation and are not the player, in join order. On a turn with no input (no player text after cleaning, and no events in the request), the speaker of the conversation's last visible turn is left out when at least one other candidate remains.
- No candidate: `DataError` `speaker: "decide" needs an NPC participant in the observation`, before any model call or record. One candidate: no model call.
- The decide call: the speakers' system message; L3 every visible turn of the conversation from its `folded_through`, witness `""`, oldest left out until it fits (no fold); no memories, no retrieval; the tail as a resolve tail shows the observation, events and player line, then `# Who may speak` with `- <name> (<id>)` lines, then `# Task` and the pack's `decide` text with `{player}` filled.
- Its schema: `{"type": "object", "properties": {"speaker": {"enum": [ids]}}, "required": ["speaker"], "additionalProperties": false}`; `maxTokens` = the largest token count of `{"speaker":"<id>"}` over the candidates plus 8; the resolve call's sampling (seed `core::SamplingSeed(seed, turn, core::CallKind::Resolve, 1 << 22)`); no thinking.
- A refused answer: the candidate who spoke least recently in the conversation (never spoke = least recent; ties in candidate order); reasons kept in `timings.decide.refused` as `the output is not valid JSON: ...` or `<pointer or "the output">: <message>`.
- `timings.decide`: the call's `CallTelemetry`, only when a decide call ran.
- The pack's seventh text: key `decide`, after `turn_cue`, variable `{player}`, default `Choose who speaks next: the character listed above who would most naturally speak now. Reply with the JSON object only.`
- `HostedCallbacks::onSpeaker`: `std::function<void(const std::string&)>`, called once per turn with the speaker id before the resolve call, for named speakers too.
- API version `1.4.0`.
- Morpheus's own game is not touched: `"[turn]"`, `"[replay]"`, `"[context]"` stay green (the only `[context]` change is one new hosted test).
- CLAUDE.md coding rules: fail fast; comments that read top to bottom; a private helper only with more than one caller (local lambdas are fine); class layout order; constants ALL_CAPS and grouped; members `_camelCase`; members initialized at declaration; ASCII only; tests first.
- Catch2 cannot stringify `std::string_view` in this build: wrap a string_view in `std::string(...)` inside a CHECK.
- Commit trailer: `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`.

Commands (Git Bash, repo root): `cmake --preset tests` after adding a file; build `cmake --build --preset tests`; one tag `./build/tests/bin/Debug/morpheus_tests.exe "[decide]"`; suite `ctest --preset tests`. Foreground builds only, one at a time; leave nothing running.
