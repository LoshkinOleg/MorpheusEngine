# The host's feedback on the integration (2026-11-16) - Design

Date: 2026-11-16. Source: the Skyrim/Mantella session's reply to `.superpowers/briefs/2026-11-16-morpheus-integration-report.md`
(received by the owner as `morpheus-reply-2026-11-16.md`; its sections are cited as "the reply"). The owner asked for
this last batch and left the design calls to the controller; they are recorded below.

## Goal

Make Morpheus build and pass on the host's Linux setup; give hosted lore per-actor scopes and observation-gated
visibility (the reply's needs A and B, "free talk is the feature"); and make the review of a hosted turn keep the
conversation's prompt cache (the reply's answer to Q6).

## Decisions (controller, on the owner's instruction)

- Linux: the `linux-*` presets name no compiler (CMake takes `CC`/`CXX`, so `clang-20` or any other works); the README
  shows `CC=clang-20 CXX=clang++-20`. Every loop that copies a `std::string` per element is fixed (the clang
  `-Wrange-loop-construct` warning is an error under `-Werror`). The Linux-only test failure is diagnosed and fixed on
  Ubuntu 24.04 (a Docker container with clang 18 in place of the host's clang 20).
- Need A (scopes) and need B (conditions) as the reply proposes, generic, on host lore; key names `scopes`, `knows`
  and `requires`.
- Need C (per-turn enum values): not built. The reply leans to classifying the topic in the fork (option 2); the engine
  can add option 1 later without breaking anything.
- Need D (skip the resolve call when the only allowed action has no parameters): not built; the reply will measure
  first.
- Q6: the review becomes a continuation of the speak prompt, as the reply prefers.

## 1. Linux

- `CMakePresets.json`: `linux-tests` and `linux-release` drop `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`. The README's
  Linux section: `CC=clang-20 CXX=clang++-20 cmake --preset linux-tests` (any clang 17+ or the host's default compiler).
- `for (const std::string x : ...)` over a list of `std::string` becomes `const std::string&` (tests/server/
  RunRegistryTests.cpp and any other site clang reports); a list of string literals keeps a value loop (it constructs,
  it does not copy).
- The test "A stop from another thread ends LlamaServerBackend::Complete while the server sends nothing" (diagnosed in
  an Ubuntu 24.04 container with clang 18): the fake server never received `/completion` before the stop at 200 ms.
  The backend's HTTP clients did not set `TCP_NODELAY`, so on Linux each small request it sends in two writes (headers,
  then body) waited for the peer's delayed ACK, about 40 ms a request, and the several requests before `/completion`
  took longer than 200 ms. With `TCP_NODELAY` the test passes (0.23 s, 3 of 3 runs). So:
  - `LlamaServerBackend`'s clients set `TCP_NODELAY` (a real latency fix on Linux for every request to llama-server;
    `morpheus_server`'s own HTTP server already sets it);
  - the test waits until the fake has received `/completion` before it stops the call, so it does not depend on how
    fast the requests before it are.

## 2. Lore scopes and conditions

### Data

- A host lore entry (`{"subject", "data", "always_on"}`) gains two optional keys:
  - `scopes`: an array of non-empty strings. Absent or empty: everyone may retrieve the entry.
  - `requires`: `{"pointer": <JSON pointer into the observation>, "min"?: <number>, "max"?: <number>, "equals"?: <any
    JSON value>}`, with at least one of `min`, `max`, `equals`. The entry is visible on a turn only when the
    observation holds a value at the pointer that meets every comparison given (`min` and `max`: a number, inclusive;
    `equals`: JSON equality). A missing value, or a value of another type, means not visible.
- An observation actor gains an optional `knows`: an array of non-empty strings (the actor's knowledge scopes). Absent:
  none (the actor retrieves only unscoped lore).
- `core::LoreEntry` gains `std::vector<std::string> scopes` and `std::optional<core::LoreCondition> condition`
  (`requires` is a C++ keyword); `core::LoreCondition {std::string pointer; std::optional<double> min, max;
  std::optional<nlohmann::json> equals; bool Holds(const nlohmann::json& observation) const;}`. `core::Actor` gains
  `std::vector<std::string> knows`. Morpheus's own game sets neither (its lore.csv has no such columns).
- Checks at `open` (DataError naming the pointer, as the other lore checks do): `scopes` not an array of non-empty
  strings; `requires` not an object, an unknown key in it, a `pointer` that is not a JSON pointer string, no
  comparison, a `min` or `max` that is not a number; an `always_on` entry with `scopes` ("an always-on entry is in every
  speaker's prompt: scopes apply to retrieved entries"). Observe: `knows` not an array of non-empty strings is a fault
  at `/actors/<i>/knows`.
- `HostDataToJson` writes `scopes` and `requires` only for entries that have them, so host data without them keeps its
  hash.

### Use

- Retrieval for speaker X on a turn: a lore entry is a candidate only when its condition holds on the turn's
  observation and its scopes are empty or share a scope with X's `knows` in that observation. `MemoryIndex::Retrieve`
  gains an optional lore filter (a predicate on the lore entry's subject); Morpheus's own game passes none.
- Always-on lore (in every prompt's system message, L1): an entry is shown when its condition holds on the
  observation the prompt is built from. A condition change (a quest stage reached) changes the system message, so that
  prompt is re-read once; scopes are refused on always-on entries (above), so L1 is the same for every speaker of a
  turn and stays cached across speakers.
- The decide call's system message and the fold and condense calls' (which reuse the turn prompt's system message)
  follow the same rule.

## 3. The review as a continuation of the speak prompt

- The review call's prompt is the speak prompt, then the first line as an assistant message, then one user message:
  the pack's `review` text, then (for a refused proposal) a blank line and `HOSTED_REVIEW_REFUSED`, then a blank line
  and `HOSTED_REVIEW_INSTRUCTION`. The review sees the conversation, the requested action (the resolve output above)
  and the line, and the conversation's prompt cache stays: only the line and the review message are new.
- `HOSTED_REVIEW_RULES` (the pack's default `review`) is reworded for its new place:

```
# Review
Check the line above, the speaker's spoken words, against these rules. Reply with one JSON object and nothing else: "consistent" is false when the line breaks a rule, true otherwise, and "problems" names each broken rule in one short sentence, at most three (empty when consistent).
- The line is only what the speaker says aloud: no name or label in front, no action in brackets, no narration or description of actions, no quotation marks around it.
- The line agrees with the action the speaker requested in the JSON object above: it does not promise or describe a different action, and it does not present a refused action as done.
- The line does not state an outcome the host has not reported.
Tone, style and details the rules do not decide are not problems.
```

- `HOSTED_REVIEW_REFUSED`: `The host refused that action: the speaker takes no action.`
- Budget: a turn with `review` reserves room for the continuation in its resolve prompt's fit: the review message (with
  the refusal note, its larger form), `review_max_tokens`, `review_thinking_tokens` and `REVIEW_TEMPLATE_MARGIN` (16)
  tokens for the two extra messages' chat-template tokens are added to what the prompt reserves after the tail (as the
  narrate allowance is), so the transcript budget shrinks by that much on reviewed turns. A turn without `review` is
  unchanged.
- `turn::HostedReviewPrompt` is replaced by `turn::HostedReviewMessage(std::string_view rules, bool refused)` (the user
  message above). The respeak stays as it is (the resolve prompt and output, then the correction message): it shares
  the cached prefix up to the resolve output.
- The pack's `review` text keeps its key and no variable; it is now a user message, not a system message. The README
  says so.
- The G3 spec (`2026-11-16-hosted-review-design.md`) section 2 step 2 and "Out of scope" are updated: the cache cost is
  gone.

## 4. API and docs

- API 1.6.0 (additive): lore `scopes` and `requires`, actor `knows`; the review keeps the cache (a behaviour change, no
  shape change).
- README (`morpheus_server`: lore scopes and conditions, `knows`; the Review paragraph without the cache cost and with
  the continuation), CHANGELOG 1.6.0, `tests/hosted/fixtures/README.md` (the lore and actor keys), the hosted-world
  spec's lore and observation sections (pointing here).

## Tests (written first)

- Linux: the fast suite builds with no warning and passes on Ubuntu 24.04 with clang (in Docker), and on Windows.
- Data: each new key read, written and round-tripped; host data without them hashes as before; each fault named;
  `knows` read by Observe, its faults; an always-on entry with scopes refused.
- `LoreCondition::Holds`: min, max, both (inclusive), equals (a string, a number, an object), a missing value, a value of
  another type.
- Retrieval: with the filter, a scoped entry is found for an actor that knows the scope and not for one that does not;
  an entry whose condition fails is not found; unscoped entries are found by everyone; with no filter (the owned game)
  nothing changes.
- Hosted turns: an NPC whose `knows` holds `magic` retrieves a `magic`-scoped entry that an NPC with no `knows` does not; an always-on
  entry gated by `/quests/MG03` >= 40 is absent from the system message at stage 30 and present at 40.
- Review: the review call's messages are the speak call's, then the first line, then the review message; the refusal
  note for a refused proposal; a reviewed turn's transcript budget is smaller by the reserve; every existing review case
  still holds.
