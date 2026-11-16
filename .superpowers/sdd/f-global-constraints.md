# Global constraints (sub-project F, the host's prompt pack)

Verbatim from docs/superpowers/plans/2026-11-16-prompt-pack.md. Spec: docs/superpowers/specs/2026-11-16-prompt-pack-design.md.


- The pack's keys and variables, exactly: `system` (no variable), `resolve` `{speaker}` `{player}`, `speak` `{speaker}` `{player}`, `turn_cue` `{speaker}` `{player}`, `summarize` `{actor}`, `condense` `{actor}`.
- A variable is `{` followed by one or more of `a-z` and `_`, then `}`. Every other brace is literal text.
- Error texts, exactly: `prompts/<key>: unknown text; the pack's texts are system, resolve, speak, turn_cue, summarize, condense`; `prompts/<key>: empty`; `prompts/<key>: unknown variable {<name>}; this text takes {speaker}, {player}` (or `takes no variable`, or `takes {actor}`); `prompts/system: <n> tokens, over engine_rules_cap <cap>`; shape: `host data at /prompts: 'prompts' must be a JSON object of texts`, `host data at /prompts/<key>: a prompt text must be a string`.
- First fault reported: unknown keys first (in key order), then each text in the table's order (empty, then its variables in text order).
- `HostDataToJson` writes `prompts` only when the host sent at least one text, and only the texts it sent: a run without a pack keeps its pre-F hash.
- No fallback: a text that fails a check is never replaced by the default.
- API version `1.3.0`.
- Morpheus's own game is not touched: `"[turn]"`, `"[replay]"`, `"[context]"` stay green with no change except `tests/context/HostedPromptTests.cpp`'s default-text test (Task 4).
- CLAUDE.md coding rules: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for host data); method bodies commented to read top to bottom; a private helper only with more than one caller; class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS; members `_camelCase`; initialize members at declaration; ASCII only in source and docs; tests written first.
- Commit trailer: `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`.

Commands (Git Bash, repo root): build `cmake --build --preset tests`; run one tag `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`; whole suite `ctest --preset tests`. A new .cpp under `tests/` is compiled in automatically; a new .cpp under `src/turn/` must be added to `src/turn/CMakeLists.txt`. Re-run `cmake --preset tests` after adding a file.
