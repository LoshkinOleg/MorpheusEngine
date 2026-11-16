# The Host's Feedback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Mantella host's last batch: Morpheus builds and passes on Linux, and the llama-server client sets TCP_NODELAY. Hosted lore gains per-actor scopes and observation-gated visibility. The review of a hosted turn becomes a continuation of the speak prompt, so it keeps the conversation's prompt cache.

**Architecture:**
- Task 1: the Linux presets, a loop copy, `LlamaServerBackend`'s clients, a robust test.
- Task 2: the data: `core::LoreCondition`, `LoreEntry::scopes` and `condition`, `Actor::knows`, read and checked.
- Task 3: their use. `MemoryIndex::Retrieve` gains a lore filter; `HostedSession` filters retrieval by the speaker's `knows` and the observation, and always-on lore by the observation. API 1.6.0, docs.
- Task 4: the review continuation, with `turn::HostedReviewMessage` and a budget reserve on reviewed turns.

**Tech Stack:** C++20, nlohmann::json, cpp-httplib, Catch2, CMake presets.

Spec: `docs/superpowers/specs/2026-11-16-host-feedback-design.md` (sections 1-4 govern; read the section your task names).

## Global Constraints

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

---

### Task 1: Linux

**Files:** `CMakePresets.json`, `README.md` (Build (Linux)), `tests/server/RunRegistryTests.cpp`, `src/llamaserver/LlamaServerBackend.cpp` (`MakeClient`), `tests/llamaserver/LlamaServerCompleteTests.cpp`.

- [ ] **Step 1:** `CMakePresets.json`: remove `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` from `linux-tests` and `linux-release`. README's Linux section: the configure commands become `CC=clang-20 CXX=clang++-20 cmake --preset linux-tests` (and `linux-release`), with one sentence: the presets name no compiler, so `CC`/`CXX` (any clang 17 or later, versioned names included) or the system default is used.
- [ ] **Step 2:** `tests/server/RunRegistryTests.cpp` (~37): `for (const std::string bad : {std::string(), ...})` becomes `for (const std::string& bad : ...)`. Keep value loops over string literals as they are (no copy).
- [ ] **Step 3:** `src/llamaserver/LlamaServerBackend.cpp`, `MakeClient`: after `set_keep_alive(true)`, `pClient->set_tcp_nodelay(true);` with a comment: each request goes out in small writes (headers, then body); without TCP_NODELAY, Linux holds the second until the peer's delayed ACK, about 40 ms a request.
- [ ] **Step 4:** `tests/llamaserver/LlamaServerCompleteTests.cpp`, "A stop from another thread ends LlamaServerBackend::Complete while the server sends nothing": the stopper thread waits until `chat.RequestsTo("/completion")` is non-empty (polling every 5 ms, up to 5 s), then requests the stop, instead of sleeping 200 ms; the `elapsed < 5 s` check stays. Add a test against the fake server that the backend's requests set `TCP_NODELAY` only if the fake can observe it cheaply; otherwise rely on the Linux run (say which in the report).
- [ ] **Step 5:** `cmake --build --preset tests && ctest --preset tests`. Commit: "Linux: presets take CC/CXX, a loop copy, TCP_NODELAY on llama-server clients, a stop test that waits for the request".

(The controller verifies the Linux build and the fast suite in an Ubuntu container after this task.)

---

### Task 2: Lore scopes and conditions: the data

**Files:** `src/core/World.h` (and `World.cpp` for `Holds`), `src/core/HostedRecord.h`, `src/core/HostedRecord.cpp` (`ReadActors`), `src/turn/HostedWorld.cpp` (`HostDataToJson`, `HostDataFromJson`, the constructor's lore checks), `src/turn/HostedWorld.h` (comments); tests `tests/core/LoreConditionTests.cpp` (new), `tests/hosted/HostedWorldTests.cpp`.

- [ ] **Step 1: Tests first.**
  - `LoreCondition::Holds` (`[lore]`): `{"/quests/MG03", min 40}` holds for 40 and 55 and not for 30; `max` inclusive; `min` and `max` both; `equals` with a string, a number and an object; a missing pointer or a string where `min` is given does not hold.
  - Host data (`[hosted][lore]`): an entry with `scopes` and `requires` round-trips through `HostDataFromJson`/`HostDataToJson` exactly. Host data without them keeps the pinned hash `35856ce76ac0af2e` (the existing pin test). Faults name their pointer: `/lore/<i>/scopes` not an array of non-empty strings; `/lore/<i>/requires` not an object or with an unknown key; `/lore/<i>/requires/pointer` not a string or not a JSON pointer (the pointer check may live in HostedWorld like the clock's); no comparison; a non-number `min` or `max`; an always-on entry with scopes (`lore at /<i>/scopes: an always-on entry is in every speaker's prompt: scopes apply to retrieved entries`).
  - Observe: an actor's `knows` is read into `Actor::knows`; a non-array or an empty string is a fault at `/actors/<i>/knows` (`'knows' must be an array of non-empty strings`).
- [ ] **Step 2:** Implement per the spec's section 2 "Data" and Global Constraints. Use the existing fault and DataError message styles of each reader (`host data at <pointer>: ...` in `HostDataFromJson`, `lore at /<i>/...` in the HostedWorld constructor, `observation at ...` in Observe). `LoreCondition::Holds` reads the value with `nlohmann::json::json_pointer` (`contains` first); a pointer that is not well-formed is refused at open, so `Holds` may treat a parse failure as a ContractError.
- [ ] **Step 3:** `cmake --build --preset tests --clean-first && ctest --preset tests`. Commit: "Host lore: scopes and requires on entries, knows on actors; read, written and checked".

---

### Task 3: Lore scopes and conditions: retrieval and prompts, API 1.6.0

**Files:** `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp` (`Retrieve`), `src/turn/HostedSession.cpp` (the turn's retrieval, `Pass`, the decide prompt's lore), `src/server/Api.h` (`API_VERSION`); tests `tests/turn/RetrievalTests.cpp`, `tests/hosted/HostedLoreTests.cpp` (new), every `"1.5.0"` under `tests/server/`; docs README, CHANGELOG, `tests/hosted/fixtures/README.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`.

- [ ] **Step 1: Tests first.**
  - `Retrieve` with a lore filter skips lore entries the filter refuses (by subject) and keeps others; without one, nothing changes.
  - Hosted turns with host lore `{"subject": "Wards", "data": "...", "scopes": ["magic"]}`, `{"subject": "Bread", "data": "..."}` and an always-on `{"subject": "The College", "data": "...", "always_on": true, "requires": {"pointer": "/quests/MG03", "min": 40}}`. An observation where Hulda has `knows: ["magic"]` and Saadia none, with `quests.MG03` at 30 or 40. Check:
    - Hulda's turn about wards retrieves "Wards"; Saadia's does not; both can retrieve "Bread".
    - At stage 30 the system message has no "The College"; at 40 it does.
    - The decide prompt and a fold prompt follow the same always-on rule.

    Use the existing retrieval-asserting hosted tests (grep `timings["retrieval"]` or `retrieved` in tests/hosted) for how to observe hits.
- [ ] **Step 2:** `MemoryIndex::Retrieve` gains a last parameter `const std::function<bool(std::string_view subject)>& loreVisible = {}`. With it, a lore entry is searched only when it returns true. Comment it. `HostedSession`:
  - Build the predicate per turn from `_world.Lore()`, the turn's `observation.json` and the speaker's `knows` (from the observation's actors): visible when `condition` holds (if any) and `scopes` is empty or shares one with `knows`.
  - Pass it to the turn's `Retrieve`.
  - `Pass` and the decide prompt keep only the always-on entries whose condition holds on the observation they are built from (`inputs.observation` / `observation.json`).
- [ ] **Step 3:** API 1.6.0 (`Api.h`; `"1.5.0"` pins under tests/server become `"1.6.0"`). CHANGELOG `## API 1.6.0`:
  - lore `scopes` and `requires`, actor `knows`, with the visibility rule;
  - the review becomes a continuation of the speak prompt (Task 4; write the line now).

  README `morpheus_server`:
  - `api_version` 1.6.0 (the sentence and the SSE example);
  - a **Lore.** paragraph (scopes, knows, requires, the rule, always-on with requires but no scopes).

  `tests/hosted/fixtures/README.md`: the lore keys and `knows`. The hosted-world spec's lore and observation sections: one line each pointing to the host-feedback spec.
- [ ] **Step 4:** `cmake --build --preset tests --clean-first && ctest --preset tests`. Commit: "Hosted lore: scoped and gated retrieval, gated always-on lore; API 1.6.0".

---

### Task 4: The review as a continuation of the speak prompt

**Files:** `src/turn/EngineText.h` (`HOSTED_REVIEW_RULES` reworded, `HOSTED_REVIEW_REFUSED`), `src/turn/Review.h`, `src/turn/Review.cpp` (`HostedReviewPrompt` -> `HostedReviewMessage`), `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp` (the review step; the reserve on reviewed turns); tests `tests/turn/ReviewTests.cpp`, `tests/hosted/HostedReviewTests.cpp`; docs README (Review paragraph), `docs/superpowers/specs/2026-11-16-hosted-review-design.md`.

- [ ] **Step 1: Tests first.**
  - `HostedReviewMessage("RULES", false) == "RULES\n\n" + HOSTED_REVIEW_INSTRUCTION`.
  - `HostedReviewMessage("RULES", true) == "RULES\n\n" + HOSTED_REVIEW_REFUSED + "\n\n" + HOSTED_REVIEW_INSTRUCTION`.
  - The new default rules text, verbatim.
  - In `HostedReviewTests`:
    - the review call's messages are the speak call's messages, then `{Assistant, <first line>}`, then `{User, HostedReviewMessage(rules, refused)}`;
    - a refused proposal gets the refusal note;
    - a host pack's `review` text is that user message's start.
    - A reviewed turn's resolve prompt has a smaller transcript budget than an unreviewed one by the reserve: compare `LastAssembly().transcriptBudget` for the same turn with and without `review`. With the fake's token count, the difference is `tokens(HostedReviewMessage(rules, true)) + reviewMaxTokens + reviewThinkingTokens + 16`.
    - Every other existing review case still holds; adjust the ones that built the old standalone prompt.
- [ ] **Step 2:** Implement per the spec's section 3 and Global Constraints:
  - `HostedReviewMessage(std::string_view rules, bool refused)` replaces `HostedReviewPrompt`; remove the old function and its tests.
  - The review call:

    ```cpp
    inference::Prompt reviewPrompt = speakPrompt;
    reviewPrompt.messages.push_back({inference::Role::Assistant, <the first line as the speak call wrote it>});
    reviewPrompt.messages.push_back({inference::Role::User,
                                     HostedReviewMessage(_world.Prompts().review, !adjudication.requested)});
    ```

    Use the speak call's raw text for the assistant message, as the respeak uses the resolve output verbatim, so the cache matches what the model wrote.
  - The reserve: when `request.review`, the turn's fit passes a budget whose `narrateAllowance` is raised by the reserve. Thread it through `Pass` (one parameter, default 0) to `AssembleHostedPrompt`. The speak message's own check against `narrate_allowance` keeps using the settings' value. Comment why.
- [ ] **Step 3:** Docs:
  - README's Review paragraph: the review continues the speak prompt, so it sees the conversation and keeps the cache. Remove the cache-cost sentence; a reviewed turn's transcript budget is smaller by the reserve. The pack's `review` text is now a user message.
  - The G3 spec: section 2 step 2 and "Out of scope" updated, pointing to the host-feedback spec.
  - CHANGELOG 1.6.0's review line.
- [ ] **Step 4:** `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[review]"`, `"[hosted]"`, `ctest --preset tests`, then `cmake --build build/dev --config RelWithDebInfo`. Commit: "Hosted review: a continuation of the speak prompt, keeping the conversation's cache; a reserve on reviewed turns".

---

## Finally (controller)

- `ctest --preset tests`; `asan`; `dev` builds clean; the consumer check.
- Linux: the fast suite in the Ubuntu 24.04 container (clang 18, Ninja), from a fresh copy of the checkout, with `CC=clang-18 CXX=clang++-18 cmake --preset linux-tests`.
- Live `[server]` (the review turn's timings, and the next turn's cache reuse if printed) and `[llama]` in-process.
- The reply to the Mantella side; commit and push.
