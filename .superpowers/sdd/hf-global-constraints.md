# Global constraints (the host's feedback)

Verbatim from docs/superpowers/plans/2026-11-16-host-feedback.md.


- Linux presets name no compiler; the README shows `CC=clang-20 CXX=clang++-20 cmake --preset linux-tests`.
- `LlamaServerBackend`'s HTTP clients call `set_tcp_nodelay(true)`.
- Lore JSON keys: `scopes` (array of non-empty strings), `requires` (`{"pointer", "min"?, "max"?, "equals"?}`, at least one comparison; `min`/`max` numbers, inclusive; `equals` any JSON, compared with JSON equality). Actor key `knows` (array of non-empty strings).
- A lore entry is visible to speaker X on a turn when its condition (if any) holds on the turn's observation and its scopes are empty or share one with X's `knows`. A missing value or a value of another type at the pointer: the condition does not hold. An `always_on` entry may not have `scopes`.
- C++ names: `core::LoreCondition {std::string pointer; std::optional<double> min; std::optional<double> max; std::optional<nlohmann::json> equals; bool Holds(const nlohmann::json& observation) const;}`; `core::LoreEntry::scopes` (`std::vector<std::string>`) and `condition` (`std::optional<core::LoreCondition>`); `core::Actor::knows` (`std::vector<std::string>`).
- `HostDataToJson` writes `scopes` and `requires` only for entries that have them; host data without them keeps its hash.
- Review continuation: the review prompt is the speak prompt, the first line as an assistant message, then one user message = the pack's `review` text + (refused: `"\n\n" + HOSTED_REVIEW_REFUSED`) + `"\n\n" + HOSTED_REVIEW_INSTRUCTION`. `HOSTED_REVIEW_REFUSED` = `The host refused that action: the speaker takes no action.`. `HOSTED_REVIEW_RULES` = the spec's section 3 block, verbatim. `REVIEW_TEMPLATE_MARGIN` = 16.
- A reviewed turn reserves, on top of what a turn reserves today: the review message's tokens (its refused form), `review_max_tokens`, `review_thinking_tokens` and `REVIEW_TEMPLATE_MARGIN`.
- API version `1.6.0`.
- Morpheus's own game unchanged: `"[turn]"`, `"[replay]"`, `"[context]"` stay green (the owned retrieval passes no lore filter).
- CLAUDE.md rules: fail fast; comments that read top to bottom; a private helper only with more than one caller; constants ALL_CAPS; ASCII only; tests first; no legacy paths.
- Catch2 cannot stringify `std::string_view` here: wrap it in `std::string(...)` inside a CHECK. Your file tool may decode "\n" escapes in string literals: grep after each write.
- After changing a struct used across files (`LoreEntry`, `Actor`, `HostedInputs`), build once with `cmake --build --preset tests --clean-first`.
- Commit trailer `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the implementing model. Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/`.

Commands (Git Bash, repo root): `cmake --preset tests` after adding a file; `cmake --build --preset tests`; one tag `./build/tests/bin/Debug/morpheus_tests.exe "[lore]"`; suite `ctest --preset tests`. Foreground only, one at a time, 600000 ms timeout; leave nothing running.
