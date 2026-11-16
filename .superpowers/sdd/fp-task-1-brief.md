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

