# Final Pass (G3 and the Follow-ups) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the Skyrim/Mantella integration's remaining work: G3 (an optional per-turn review of a hosted turn's line, with one respeak), E2's two follow-ups (the clock after a rewind; no resync on a rewind that changes nothing), and the small leftovers the G1, G2 and F reviews listed.

**Architecture:** G3 adds `turn::HostedReviewPrompt` and the pack's eighth text `review` (Task 1), the review and respeak steps in `HostedSession::RunTurn` behind `HostedTurnRequest::review` (Task 2), and the server's `review` field (Task 3, API 1.5.0). Task 4 changes `RunStore::LatestClock` and `HostedSession::Rewind`. Task 5 is a cleanup of comments, docs, a call-tag header and small guards.

**Tech Stack:** C++20, nlohmann::json, SQLite, Catch2, CMake presets (`tests`, `asan`, `dev`).

Specs: `docs/superpowers/specs/2026-11-16-hosted-review-design.md` (G3), `docs/superpowers/specs/2026-11-16-rollback-design.md` (E2), `docs/superpowers/specs/2026-11-16-generation-controls-design.md` (G2).

## Global Constraints

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

---

### Task 1: The review's texts and prompt

**Files:**
- Modify: `src/turn/EngineText.h`, `src/turn/PromptPack.h`, `src/turn/PromptPack.cpp`, `src/turn/Review.h`, `src/turn/Review.cpp`
- Test: `tests/turn/PromptPackTests.cpp`, `tests/server/ApiPromptTests.cpp`, `tests/turn/ReviewTests.cpp`

**Interfaces:**
- Produces: `turn::HOSTED_REVIEW_RULES`, `turn::HOSTED_REVIEW_INSTRUCTION`, `turn::HOSTED_RESPEAK_CORRECTION`; `PromptPack::review` (`std::string`, default `HOSTED_REVIEW_RULES`); `PROMPT_TEXTS` 8 entries; `inference::Prompt turn::HostedReviewPrompt(std::string_view rules, std::string_view speakerName, std::string_view playerLine, const core::Adjudication& adjudication, const nlohmann::json& observation, std::string_view line)`.

- [ ] **Step 1: Write the failing tests**

`tests/turn/PromptPackTests.cpp`: the default test checks `pack.review == std::string(turn::HOSTED_REVIEW_RULES)` and `&pack.Text("review") == &pack.review`; every expected unknown-text list becomes `system, resolve, speak, turn_cue, decide, summarize, condense, review`; add `CHECK(PackError({{"review", "Check {speaker}."}}) == "prompts/review: unknown variable {speaker}; this text takes no variable");`.

`tests/server/ApiPromptTests.cpp`: `prompts.size() == 8`, and the expected variables gain `{"review", nlohmann::json::array()}`.

`tests/turn/ReviewTests.cpp`, append (include `core/HostedRecord.h`, `turn/EngineText.h`):

```cpp
TEST_CASE("HostedReviewPrompt lays out the speaker, the player, the action, the observation and the line",
          "[review][hosted]")
{
    core::Adjudication follow;
    follow.requested = true;
    follow.actionId = "follow";
    follow.params = nlohmann::json::object();
    const nlohmann::json observation = {{"location", "inn"}};
    const inference::Prompt prompt =
        turn::HostedReviewPrompt("RULES", "Hulda", "Aela: Follow me.", follow, observation, "Lead the way.");
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.stableCount == 1);
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == "RULES");
    CHECK(prompt.messages[1].content == "# Speaker\nHulda\n\n# Player\nAela: Follow me.\n\n# Requested action\nfollow\n\n"
                                        "# Observation\n{\"location\":\"inn\"}\n\n# Line\nLead the way.\n\n# Task\n" +
                                            std::string(turn::HOSTED_REVIEW_INSTRUCTION));

    // An action with parameters shows them; a refused one says so; no player text, no Player section.
    core::Adjudication give = follow;
    give.actionId = "give_item";
    give.params = {{"item", "Sweetroll"}};
    CHECK_THAT(turn::HostedReviewPrompt("R", "Hulda", "", give, observation, "Here.").messages[1].content,
               Catch::Matchers::ContainsSubstring("# Requested action\ngive_item {\"item\":\"Sweetroll\"}\n\n"));
    core::Adjudication refused;
    const std::string none = turn::HostedReviewPrompt("R", "Hulda", "", refused, observation, "Hm.").messages[1].content;
    CHECK(none.starts_with("# Speaker\nHulda\n\n# Requested action\nnone (the proposal was refused)\n\n"));
}

TEST_CASE("The default review rules name the speaking rules", "[review][hosted]")
{
    const std::string rules(turn::HOSTED_REVIEW_RULES);
    CHECK(rules.starts_with("# Review\nYou check one spoken line"));
    CHECK_THAT(rules, Catch::Matchers::ContainsSubstring("no name or label in front"));
    CHECK(std::string(turn::HOSTED_RESPEAK_CORRECTION) ==
          "Your previous line broke the speaking rules. Say it again without these problems:");
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (the texts, `PromptPack::review` and `HostedReviewPrompt` do not exist).

- [ ] **Step 3: Implement**

`src/turn/EngineText.h`, after the hosted condense instruction: `HOSTED_REVIEW_RULES` (a raw string, exactly the spec's section 4 block, starting `# Review` and ending `...are not problems.`), `HOSTED_REVIEW_INSTRUCTION` and `HOSTED_RESPEAK_CORRECTION` (Global Constraints' texts), each with a comment naming its use (the review call's system message, PromptPack's default `review`; the end of its user message; the respeak message's opening).

`src/turn/PromptPack.h`: `PROMPT_TEXTS` becomes `std::array<PromptText, 8>` with `{"review", {}},` after `condense`; the class gains `std::string review = std::string(HOSTED_REVIEW_RULES);` after `condense`. `src/turn/PromptPack.cpp`: `MEMBERS` 8, `&PromptPack::review` last.

`src/turn/Review.h`, add (include `<nlohmann/json.hpp>`, `core/HostedRecord.h`):

```cpp
// The review call of a hosted turn (hosted-review spec, section 2), standalone: `rules` (the run's pack `review`) as the
// system message, then one user message: "# Speaker\n<name>\n\n", "# Player\n<playerLine>\n\n" when there is one,
// "# Requested action\n<id>" (then " <params as compact JSON>" when it has parameters; "none (the proposal was
// refused)" for a refused one), "\n\n# Observation\n<compact JSON>\n\n# Line\n<line>\n\n# Task\n" and
// HOSTED_REVIEW_INSTRUCTION. stableCount is 1 (the rules).
inference::Prompt HostedReviewPrompt(std::string_view rules, std::string_view speakerName, std::string_view playerLine,
                                     const core::Adjudication& adjudication, const nlohmann::json& observation,
                                     std::string_view line);
```

`src/turn/Review.cpp`:

```cpp
inference::Prompt HostedReviewPrompt(std::string_view rules, std::string_view speakerName, std::string_view playerLine,
                                     const core::Adjudication& adjudication, const nlohmann::json& observation,
                                     std::string_view line)
{
    // The host's JSON may hold text that is not UTF-8: the replace handler keeps it from throwing.
    auto compact = [](const nlohmann::json& value) {
        return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    };
    // Who speaks, what the player said, the action the speaker requested, the scene, then the line under review.
    std::string user = std::format("# Speaker\n{}\n\n", speakerName);
    if (!playerLine.empty()) {
        user += std::format("# Player\n{}\n\n", playerLine);
    }
    user += "# Requested action\n";
    if (!adjudication.requested) {
        user += "none (the proposal was refused)";
    } else {
        user += adjudication.actionId;
        if (!adjudication.params.empty()) {
            user += " " + compact(adjudication.params);
        }
    }
    user += std::format("\n\n# Observation\n{}\n\n# Line\n{}\n\n# Task\n{}", compact(observation), line,
                        HOSTED_REVIEW_INSTRUCTION);
    inference::Prompt prompt;
    prompt.messages = {{inference::Role::System, std::string(rules)}, {inference::Role::User, std::move(user)}};
    prompt.stableCount = 1;
    return prompt;
}
```

(include `<format>`, `turn/EngineText.h`).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[review],[prompts]"`, then `ctest --preset tests`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn/EngineText.h src/turn/PromptPack.h src/turn/PromptPack.cpp src/turn/Review.h src/turn/Review.cpp tests/turn/PromptPackTests.cpp tests/server/ApiPromptTests.cpp tests/turn/ReviewTests.cpp
git commit -m "Hosted review texts and prompt: the pack's review text and HostedReviewPrompt"
```

---

### Task 2: `RunTurn` reviews the line when asked

**Files:**
- Modify: `src/turn/HostedSession.h` (`HostedTurnRequest::review`, `RunTurn`'s comment), `src/turn/HostedSession.cpp` (`RunTurn`)
- Modify: `tests/hosted/HostedTestSupport.h` (`HostedScript` answers review calls)
- Test: `tests/hosted/HostedReviewTests.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `HostedReviewPrompt`, `PromptPack::review`, `HOSTED_RESPEAK_CORRECTION`; `turn::BuildReviewGrammar`, `turn::ParseReviewOutput`, `turn::Review` (src/turn/Review.h, ResolveGrammar.h).
- Produces: `HostedTurnRequest::review` (`bool`, default false); `test::HostedScript::reviews` (`std::vector<std::string>`, answered in order to calls tagged `review`) and `reviewed` (`std::size_t`).

- [ ] **Step 1: Test support**

`tests/hosted/HostedTestSupport.h`, `HostedScript`: add `std::vector<std::string> reviews = {};` and `std::size_t reviewed = 0;`; in `Responder()`, before the grammar check, `if (spec.callTag == "review") { MORPHEUS_REQUIRE(reviewed < reviews.size(), "HostedScript: no review left"); return reviews[reviewed++]; }`. A respeak call has no grammar and takes the next of `lines`, as a speak call does. Update the struct's comment.

- [ ] **Step 2: Write the failing tests**

Create `tests/hosted/HostedReviewTests.cpp` with these cases (use `test::HostedScript`, `test::ScriptedBackend`, `test::Request`, the open.json host data and HULDA/AELA; a `HostedCallbacks` whose `onToken` appends to a string and records the calls made so far, `backend.Calls().size()`, at its first piece):

1. "Without review a turn is as before": two calls; the tokens arrive during the speak call (`backend.Calls().size()` is 2 at the first piece); no `timings.review`.
2. "A consistent review: the line is delivered once, after the review": `request.review = true`, `script.reviews = {R"({"consistent":true,"problems":[]})"}`; three calls with tags `resolve`, `speak`, `review`; the first piece arrives when 3 calls were made; the delivered text equals the committed line; `timings.review.consistent` true and `timings.review.problems` empty; no `timings.respeak`. The review call's spec: `grammar == turn::BuildReviewGrammar(true, false)`, `maxTokens == test::HostedSettings().budget.reviewMaxTokens`; its prompt equals `turn::HostedReviewPrompt(std::string(turn::HOSTED_REVIEW_RULES), "Hulda", "Aela: <the player text>", record->adjudication, test::FixtureObservation(), "<the first line>")`.
3. "An inconsistent review: the line is spoken once more with the problems": `script.lines = {"Hulda [wait]: Fine.", "Fine, I will wait."}`, `script.reviews = {R"({"consistent":false,"problems":["The line starts with a label."]})"}`; four calls (`resolve`, `speak`, `review`, `respeak`); the respeak call's last message is `std::string(turn::HOSTED_RESPEAK_CORRECTION) + "\n- The line starts with a label.\n\n" + <the speak call's last message>`, and its other messages equal the speak call's; the committed and delivered line is "Fine, I will wait."; "Hulda [wait]" was never delivered; `timings.respeak` present.
4. "An unreadable review keeps the first line": `script.reviews = {"not json"}`; three calls; the first line committed and delivered; `timings.review.error` non-empty and no `consistent`.
5. "A respeak message over narrate_allowance drops problems": settings with `narrateAllowance` just above the speak message's size (count it with the fake: `(size + 3) / 4`), a review naming two long problems; the respeak's last message holds neither long problem but still starts with the correction when it fits, else equals the plain speak message (compute which from the fake count in the test and check that branch).
6. "A host pack's review text is the review call's system message": host data with `prompts = {{"review", "MY RULES"}}`; the review call's first message is "MY RULES".
7. "A stop during the review commits nothing": a responder that requests stop on the review call; `RunTurn` returns nullopt; no turn stored; nothing delivered.

- [ ] **Step 3: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: compile error (`review` is not a member of `HostedTurnRequest`).

- [ ] **Step 4: Implement**

`src/turn/HostedSession.h`, `HostedTurnRequest`, last member: `bool review = false; // Review the line before it is delivered (hosted-review spec); off: as before.` `RunTurn`'s comment gains a sentence: with `review`, the line is held back, reviewed, spoken once more with the review's problems when it finds any, then delivered to `onToken` in one piece; `timings.review` and `timings.respeak`.

`src/turn/HostedSession.cpp`, `RunTurn` (include `turn/Review.h` and `turn/ResolveGrammar.h` if missing):
- The speak call passes `request.review ? inference::TokenCallback() : callbacks.onToken` instead of `callbacks.onToken`, and the comment says the line streams only without a review.
- After the speak call's cancel check, before "4. Commit the record":

```cpp
    // 3b. The review (hosted-review spec), when the request asks for it: the line was held back; the model reviews it
    // against the requested action and the speaking rules; problems get one respeak; the line is then delivered.
    std::string line = core::Trim(spoken.text);
    nlohmann::json reviewTelemetry = nullptr;
    nlohmann::json respeakTelemetry = nullptr;
    if (request.review) {
        inference::GenerationSpec reviewSpec;
        reviewSpec.grammar = BuildReviewGrammar(true, false);
        reviewSpec.maxTokens = _settings.budget.reviewMaxTokens;
        reviewSpec.thinkingMaxTokens = _settings.budget.reviewThinkingTokens;
        reviewSpec.callTag = "review";
        reviewSpec.sampling =
            SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Review, 0));
        const inference::Completion reviewed =
            _pBackend->Complete(HostedReviewPrompt(_world.Prompts().review, pSpeaker->name, tail.playerLine,
                                                   adjudication, observation.json, line),
                                reviewSpec, {}, stop);
        if (reviewed.cancelled) {
            return std::nullopt;
        }
        // An output that cannot be read is the model's mistake: the line stands, and the error is kept.
        const Review review = ParseReviewOutput(reviewed.text, true, false);
        reviewTelemetry = CallTelemetry(reviewed, reviewSpec);
        if (review.consistent) {
            reviewTelemetry["consistent"] = *review.consistent;
        }
        reviewTelemetry["problems"] = review.problems;
        if (!review.error.empty()) {
            reviewTelemetry["error"] = review.error;
        }
        if (review.consistent == false) {
            // The respeak message: the correction, the problems that fit narrate_allowance (dropped from the end), a
            // blank line and the speak instruction; when even the bare correction is too long, the speak instruction.
            std::vector<std::string> problems = review.problems;
            auto respeakMessage = [&] {
                std::string text = std::string(HOSTED_RESPEAK_CORRECTION) + "\n";
                for (const std::string& problem : problems) {
                    text += "- " + problem + "\n";
                }
                return text + "\n" + speakMessage;
            };
            std::string message = respeakMessage();
            while (_pBackend->CountTokens(message) > _settings.budget.narrateAllowance && !problems.empty()) {
                problems.pop_back();
                message = respeakMessage();
            }
            if (_pBackend->CountTokens(message) > _settings.budget.narrateAllowance) {
                message = speakMessage;
            }
            inference::Prompt respeakPrompt = assembly->prompt;
            context::ExtendForNarrate(respeakPrompt, resolved.text, message);
            inference::GenerationSpec respeakSpec = speakSpec;
            respeakSpec.callTag = "respeak";
            respeakSpec.sampling =
                SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Renarrate, 0));
            const inference::Completion respoken = _pBackend->Complete(respeakPrompt, respeakSpec, {}, stop);
            if (respoken.cancelled) {
                return std::nullopt;
            }
            respeakTelemetry = CallTelemetry(respoken, respeakSpec);
            line = core::Trim(respoken.text);
        }
        // The line the turn commits reaches the host now, in one piece.
        if (callbacks.onToken && !line.empty()) {
            callbacks.onToken(line);
        }
    }
```

- `record.line = line;` (instead of `core::Trim(spoken.text)`), and after `timings["speak"]`: `timings["review"]` and `timings["respeak"]` when not null.

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[review]"`, then `"[hosted]"`, then `ctest --preset tests`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTestSupport.h tests/hosted/HostedReviewTests.cpp
git commit -m "Hosted review: a turn asked to review holds its line back, reviews it, respeaks once on problems"
```

---

### Task 3: The server's `review`, API 1.5.0, docs

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (the turn body and `done`)
- Test: `tests/server/ApiReviewTests.cpp` (new), every `"1.4.0"` pin under `tests/server/`, `tests/server/ServerLiveTests.cpp` (one live case)
- Docs: `README.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`

**Interfaces:**
- Consumes: `HostedTurnRequest::review`; `test::HostedScript::reviews` (Task 2).

- [ ] **Step 1: Write the failing tests**

Create `tests/server/ApiReviewTests.cpp` (model it on `tests/server/ApiDecideTests.cpp`):
- "A review turn streams the action, then the line after the review, then done with timings.review": open, a conversation of Hulda and Aela, a turn body with `{"speaker": HULDA, "player_text": "Wait here.", "review": true, "observation": ...}`, `script.reviews = {R"({"consistent":true,"problems":[]})"}`; the events are `action`, then `token`s and `sentence`s whose text is the line, then `done` whose `timings` contain `review` (and no `respeak`), `api_version` "1.5.0".
- "A malformed review is a 400": `"review": "yes"` answers 400 with the message `review: expected true or false`.

Change every `"1.4.0"` under `tests/server/` to `"1.5.0"`.

`tests/server/ServerLiveTests.cpp`: in the conversation fixture case, send the fixture's last turn with `"review": true` (add the key to its body) and keep the existing checks (it completes and ends with done); say so in a comment.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: the review turn streams tokens during speak and `done` lacks `review`; the version pins fail; "yes" is accepted.

- [ ] **Step 3: Implement**

`src/server/Api.h`: `API_VERSION = "1.5.0"`. `src/server/Api.cpp`, the turn route, after `allowed_actions`:

```cpp
                    if (json.contains("review")) {
                        if (!json.at("review").is_boolean()) {
                            throw core::DataError("review: expected true or false");
                        }
                        request.review = json.at("review").get<bool>();
                    }
```

and the `done` event's `timings` gain `review` and `respeak` when the record's timings contain them (as `decide`).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all pass.

- [ ] **Step 5: Docs**

- `CHANGELOG.md`, above `## API 1.4.0`: `## API 1.5.0` "The review of a hosted turn (design: `docs/superpowers/specs/2026-11-16-hosted-review-design.md`). Only additions." with bullets: a turn's optional `review` (what it checks, one respeak, the line streams after the review, its cost); `done`'s `timings.review` and `timings.respeak`; the prompt pack's `review` text.
- `README.md`, `morpheus_server` section: `api_version` 1.5.0 (sentence and SSE example); the turn route's body names `review?`; a **Review.** paragraph (what is checked, that the line is held back and arrives after the review, one respeak, the timings, the cost: one review call and sometimes one more speak call, so measure it before enabling it widely); the Prompt pack paragraph lists `review` (no variable).
- Server spec: the turn route's body gains `review` (added in 1.5.0, G3); the `api_version` line 1.5.0; `done`'s timings line names `decide`, `review` and `respeak`; the Documentation section's CHANGELOG bullet adds 1.5.0.
- Prompt-pack spec, section 1's table: a `review` row (the review call's system message; no variable; added by G3).

- [ ] **Step 6: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp tests/server/ApiReviewTests.cpp tests/server/*.cpp README.md CHANGELOG.md docs/superpowers/specs/2026-11-16-morpheus-server-design.md docs/superpowers/specs/2026-11-16-prompt-pack-design.md
git commit -m "Server: a turn's review, timings.review and respeak in done, API 1.5.0; docs"
```

---

### Task 4: E2's follow-ups: the clock after a rewind, no resync for a rewind that changes nothing

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp` (`LatestClock`), `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp` (`Rewind`), `docs/superpowers/specs/2026-11-16-rollback-design.md`
- Test: `tests/store/RollbackStoreTests.cpp`, `tests/hosted/HostedRewindTests.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/store/RollbackStoreTests.cpp` (use the file's existing helpers for a run with clocked records): a run whose latest record is clocked 4.0; `ApplyRollback(2.5)` makes `LatestClock() == 2.5` (it was 2.0, the highest visible record, before the fix); `UndoRollback()` makes it 4.0 again; `ApplyRollback(2.5)`, then an abandon (as the file's abandon tests do), then `ApplyRollback(2.4)` makes it 2.4 (the most recent rollback, not the abandoned 2.5).

`tests/hosted/HostedRewindTests.cpp`: after a rewind to 2.5 of a run whose records reach 4.0 (with a clocked turn at 2.0), `RecordEvents` with one event: the stored event's clock is 2.5 (read it through the store's events as the file's other tests read clocks). And: a rewind that changes nothing (`gameTime` at or after the latest clock with no open rollback) returns `rewound == false` and makes no backend call (`backend.Calls()` and `backend.EmbeddedTexts()` unchanged).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[rollback],[rewind]"`
Expected: LatestClock is 2.0 after the rollback; the event is dated 2.0.

- [ ] **Step 3: Implement**

`src/store/RunStore.cpp`, `LatestClock`'s query gains, in the union: `SELECT game_time FROM rollbacks WHERE id = (SELECT MAX(id) FROM rollbacks WHERE status <> 'undone')`, and the comment says why: after a rewind to T, the run is at T, so a record with no observation of its own is dated T, not with the last record before T (an undone rollback is gone; an abandoned one still marks the time play went on from). `RunStore.h`'s `LatestClock` comment says the same.

`src/turn/HostedSession.cpp`, `Rewind`: after `result.rewound` is computed, `if (!result.rewound) { return result; }` with the comment "Nothing changed: the index is as it was." before `SyncHistory()`. `HostedSession.h`'s `Rewind` comment notes it.

Rollback spec, section 1's last bullet ("The run's latest seen clock ..."): add "raised to the game time of the most recent rollback that is not undone, when that is higher" and the reason; section 3: a rewind that changes nothing does not resync.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[rollback],[rewind],[hosted]"`, then `ctest --preset tests`
Expected: all pass (the reload fixture included).

- [ ] **Step 5: Commit**

```bash
git add src/store/RunStore.h src/store/RunStore.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp docs/superpowers/specs/2026-11-16-rollback-design.md tests/store/RollbackStoreTests.cpp tests/hosted/HostedRewindTests.cpp
git commit -m "Rewind: records after a rewind are dated at its time; a rewind that changes nothing does not resync"
```

---

### Task 5: Leftovers from the G1, G2 and F reviews

**Files:** as each item says. Tests first where there is behaviour.

- [ ] **Step 1: Call-tag constants.** Create `src/turn/CallTag.h` with `inline constexpr std::string_view` constants `LOOKUP_TAG = "lookup"`, `RESOLVE_TAG`, `NARRATE_TAG`, `RENARRATE_TAG`, `REVIEW_TAG`, `SUMMARIZE_TAG`, `CONDENSE_TAG`, `DECIDE_TAG`, `SPEAK_TAG`, `RESPEAK_TAG` (a comment: the call tags the turn layer sets, generation-controls spec section 2). Use them where the tags are set (`Session.cpp`, `HostedSession.cpp`, `MemoryUpkeep.cpp`, as `std::string(...)`) and where they are read (`Replay.cpp`'s `CallKindOf`, `app/model/ModelSeat.cpp`'s `SeatLabelOf`, `tests/hosted/HostedTestSupport.h`). Tests keep their literals (they pin the spelling).
- [ ] **Step 2: `thinking_format` error for a non-string, non-object value.** `src/core/Settings.cpp`: a `thinking_format` that is neither a string nor an object (a number, null, an array, a boolean) gets the preset message (`'thinking_format' must be "auto", "qwen", "none" or an object with open, skip, end, after_end and close, got <value as JSON>`), not "must be an object". Test in `tests/core/SettingsTests.cpp` with `5` and `null`.
- [ ] **Step 3: The settings editor keeps `thinking_format`.** `tests/app/SettingsEditTests.cpp`: a settings file with `"thinking_format": "qwen"`, an edit saved, the file re-read still has `"thinking_format": "qwen"`.
- [ ] **Step 4: Empty-text guards in the llama-server backend.** `src/llamaserver/LlamaServerBackend.cpp`: guard an empty `afterEnd` before tokenizing, as `skip` is guarded (an empty text tokenizes to no tokens without a request). Test against the fake server: a custom format with an empty `after_end` (and a `close` ending with its `end`) loads and makes no `/tokenize` request for an empty text (check the requests' bodies).
- [ ] **Step 5: Comments and docs.**
  - `src/turn/HostedSession.h`, `RunTurn`'s comment: "no events" (the decide rule) becomes "no new events".
  - `tests/turn/SamplingTests.cpp`: the SamplingFrom min_p case's tag `[sampling]` becomes `[turn]`.
  - `README.md`: the budget bullet that defines a thinking model as one whose template has `enable_thinking` says that is the `auto` rule and points to `thinking_format`; one sentence in the `thinking_format` description: a custom format on a template that has the enable_thinking switch is used over llama-server with the template's own thinking switch on (prefer `qwen` or `auto` for Qwen models).
  - Re-wrap to about 120 columns: `tests/hosted/HostedPromptPackTests.cpp` (~30), `tests/hosted/HostedWorldTests.cpp` (the two ~125-column lines), `src/context/PromptBuilder.h` (any line over 120), `src/turn/HostedSession.cpp` (any line over 120 in the decide block), `tests/hosted/HostedDecideTests.cpp` (any line over 120). Use `awk 'length > 120 {print FILENAME":"FNR": "length}'` to find them; change only lines you can wrap without changing code.
- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset tests --clean-first && ctest --preset tests`, then `cmake --build build/dev --config RelWithDebInfo` (every dev target; `src/llama` and the tools include the turn headers).
Expected: all pass, the dev build clean.

- [ ] **Step 7: Commit** (one commit per step 1-4, one for step 5)

```bash
git add src/turn/CallTag.h src/turn/Session.cpp src/turn/HostedSession.cpp src/turn/MemoryUpkeep.cpp src/turn/Replay.cpp src/app/model/ModelSeat.cpp tests/hosted/HostedTestSupport.h
git commit -m "Call tags as constants in turn/CallTag.h, used where the tags are set and read"
```

(and similarly for the others, each staging only its files).

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`; `cmake --build build/dev --config RelWithDebInfo`; the consumer check.
- Live: `[llama]` in-process (Qwen3-4B; thinking on Qwen3.5-9B) and `[server]` over llama-server (the review case included).
- The integration summary for the Skyrim/Mantella implementer; commit and push.
