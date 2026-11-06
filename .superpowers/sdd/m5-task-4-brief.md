### Task 4: The session reviews each narration and renarrates a contradiction

After narrate, the session makes one review call when `narration_check` or `compact_transcript` is on. With the check on, a first review that says `"consistent": false` restarts the narration once: `onNarrationRestarted` fires, the narration is written again from the same narrate prompt plus the correction, and its own review decides the digest. The second narration is committed whatever its review says. A cancel during the review or the renarrate cancels the turn.

**Files:**
- Modify: `src/turn/Session.h`, `src/turn/Session.cpp`
- Test: `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `turn::BuildReviewGrammar`, `turn::ParseReviewOutput`, `turn::ReviewPrompt`, `turn::Review`, `turn::RenderNarrateMessage(..., problems, ...)` (Task 3); `core::Settings::narrationCheck`, `compactTranscript`, `budget.reviewMaxTokens`, `budget.reviewThinkingTokens`, `core::CallKind::Review`, `core::CallKind::Renarrate` (Task 1); `core::TurnRecord::digest` (Task 2).
- Produces:
  - `constexpr int turn::REVIEW_RETRY_OFFSET = 1 << 23` (`turn/Session.h`).
  - `std::function<void(const std::vector<std::string>& problems)> turn::TurnCallbacks::onNarrationRestarted`.
  - Telemetry: `timings["review"]` (array; each entry is the call's `Telemetry` plus `consistent`, `problems`, `digest`, `error` when set); `timings["renarrate"]` (the renarrate call's `Telemetry` plus `"replaced"`: the first narration), only after a renarrate. `timings["narrate"]` stays the first narration's call.
  - `record.digest`: the last review's digest.

- [ ] **Step 1: Write the failing tests**

In `tests/turn/SessionTests.cpp`, add to the includes:

```cpp
#include "core/Dice.h"
#include "turn/ResolveGrammar.h"
```

Inside the anonymous namespace, after `Tail`, add:

```cpp
// A review that finds nothing wrong, with the turn's digest.
const std::string CONSISTENT = R"({"consistent":true,"problems":[],"digest":"The player walked to the lighthouse."})";

// TestSettings with the review call on: the narration check and the digest as given, and the review's budget from
// settings.example.json.
core::Settings ReviewSettings(bool check, bool digest)
{
    core::Settings settings = TestSettings();
    settings.narrationCheck = check;
    settings.compactTranscript = digest;
    settings.budget.reviewMaxTokens = 384;
    return settings;
}

// A scripted model for review tests. Resolve calls get `resolve`; review calls (their system message is REVIEW_RULES)
// get `reviews` in order; summary calls get "Summary."; narrate calls get `narrations` in order. Running out of
// replies is a test bug. The script must outlive the backends that call it.
struct ReviewScript {
    std::string resolve = {};
    std::vector<std::string> narrations = {};
    std::vector<std::string> reviews = {};
    std::size_t narrated = 0;
    std::size_t reviewed = 0;

    test::ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            if (spec.grammar.find("intent") != std::string::npos) {
                return resolve;
            }
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                MORPHEUS_REQUIRE(reviewed < reviews.size(), "ReviewScript: no review reply left");
                return reviews[reviewed++];
            }
            const std::string& last = prompt.messages.back().content;
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
                return "Summary.";
            }
            MORPHEUS_REQUIRE(narrated < narrations.size(), "ReviewScript: no narration left");
            return narrations[narrated++];
        };
    }
};
```

Append the test cases:

```cpp
TEST_CASE("A consistent review commits the narration and its digest", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {CONSISTENT}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_consistent"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest == "The player walked to the lighthouse.");
    CHECK(session.History().at(0).digest == "The player walked to the lighthouse."); // Stored with the turn.
    CHECK(restarts == 0);

    // The review is a standalone call: the rules, then the input, the outcome and the narration.
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    const inference::Prompt& review = calls[2].prompt;
    REQUIRE(review.messages.size() == 2);
    CHECK(review.messages[0].content == turn::REVIEW_RULES);
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Player\nI walk to the lighthouse.\n"));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("Applied: player.location = \"lighthouse\""));
    CHECK_THAT(review.messages[1].content, ContainsSubstring("# Narration\nYou walk to the lighthouse.\n"));
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(true, true));
    CHECK(calls[2].spec.maxTokens == 384);
    CHECK(calls[2].spec.thinkingMaxTokens == 0);
    CHECK(calls[2].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Review, 0));

    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 1);
    CHECK(reviews[0].at("consistent") == true);
    CHECK(reviews[0].at("digest") == "The player walked to the lighthouse.");
    CHECK(reviews[0].contains("prompt_tokens"));
    CHECK_FALSE(reviews[0].contains("problems"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("A narration that contradicts the outcome is narrated once more and replaced", "[session][review]")
{
    const std::string contradicts =
        R"({"consistent":false,"problems":["The climb failed, but the narration has the player reach the top."],)"
        R"("digest":"The player reached the top."})";
    const std::string fixed = R"({"consistent":true,"problems":[],"digest":"The player slipped on the cliff."})";
    ReviewScript script{CLIMB, {"You reach the top with ease.", "You slip on the wet rock."}, {contradicts, fixed}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate"), 42, backend);
    std::vector<std::vector<std::string>> restarts;
    std::string streamed;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>& problems) { restarts.push_back(problems); };
    callbacks.onNarrationToken = [&](std::string_view piece) { streamed += piece; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I climb the cliff.", callbacks, {});
    REQUIRE(record);

    // The second narration and the second review's digest are committed.
    CHECK(record->narration == "You slip on the wet rock.");
    CHECK(record->digest == "The player slipped on the cliff.");
    REQUIRE(restarts.size() == 1);
    CHECK(restarts[0] == std::vector<std::string>{"The climb failed, but the narration has the player reach the top."});
    CHECK(streamed == "You reach the top with ease.You slip on the wet rock."); // Both stream; the UI clears on the restart.

    // The renarrate is the narrate prompt with the correction before the instruction, with its own seed.
    const auto& calls = backend.Calls(); // resolve, narrate, review, renarrate, review
    REQUIRE(calls.size() == 5);
    const inference::Prompt& first = calls[1].prompt;
    const inference::Prompt& again = calls[3].prompt;
    REQUIRE(again.messages.size() == first.messages.size());
    for (std::size_t i = 0; i + 1 < first.messages.size(); ++i) {
        CHECK(again.messages[i].content == first.messages[i].content);
    }
    CHECK_THAT(first.messages.back().content, !ContainsSubstring(std::string(turn::RENARRATE_CORRECTION)));
    CHECK_THAT(again.messages.back().content,
               ContainsSubstring(std::string(turn::RENARRATE_CORRECTION) +
                                 "\n- The climb failed, but the narration has the player reach the top.\n\n" +
                                 std::string(turn::NARRATE_INSTRUCTION)));
    CHECK(calls[3].spec.grammar.empty());
    CHECK(calls[3].spec.sampling.seed == core::SamplingSeed(42, 1, core::CallKind::Renarrate, 0));
    // The second review reads the second narration, seeded apart from any first review.
    CHECK_THAT(calls[4].prompt.messages[1].content, ContainsSubstring("# Narration\nYou slip on the wet rock.\n"));
    CHECK(calls[4].spec.sampling.seed ==
          core::SamplingSeed(42, 1, core::CallKind::Review, turn::REVIEW_RETRY_OFFSET));

    // Both verdicts, the renarrate's call and the replaced text are in the telemetry.
    const nlohmann::json& reviews = record->timings.at("review");
    REQUIRE(reviews.size() == 2);
    CHECK(reviews[0].at("consistent") == false);
    CHECK(reviews[0].at("problems") ==
          nlohmann::json::array({"The climb failed, but the narration has the player reach the top."}));
    CHECK(reviews[1].at("consistent") == true);
    CHECK(record->timings.at("renarrate").at("replaced") == "You reach the top with ease.");
    CHECK(record->timings.at("renarrate").contains("generated"));
    CHECK(record->timings.at("narrate").contains("generated")); // The first narration's call stays.
}

TEST_CASE("A renarrated turn is committed whatever its second review says", "[session][review]")
{
    const std::string first = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    const std::string second =
        R"({"consistent":false,"problems":["The player still flies."],"digest":"The player flew again."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You float to the lighthouse."}, {first, second}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_renarrate_twice"), 42, backend);
    int restarts = 0;
    turn::TurnCallbacks callbacks;
    callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { ++restarts; };
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", callbacks, {});
    REQUIRE(record);
    CHECK(record->narration == "You float to the lighthouse.");
    CHECK(record->digest == "The player flew again.");
    CHECK(restarts == 1);
    CHECK(backend.Calls().size() == 5); // No second renarrate.
    CHECK(record->timings.at("review").at(1).at("consistent") == false);
}

TEST_CASE("An unreadable review commits the narration with no digest and says why", "[session][review]")
{
    ReviewScript script{MOVE, {"You walk to the lighthouse."}, {R"({"consistent":tr)"}}; // Cut off.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                 NewRunFile("session_review_unreadable"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(record->narration == "You walk to the lighthouse.");
    CHECK(record->digest.empty());
    CHECK(backend.Calls().size() == 3); // As if the review had found nothing: no renarrate.
    const nlohmann::json& review = record->timings.at("review").at(0);
    CHECK_THAT(review.at("error").get<std::string>(), ContainsSubstring("not complete JSON"));
    CHECK_FALSE(review.contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With only the compact transcript on, the review writes the digest and never renarrates", "[session][review]")
{
    // The output says the narration contradicts the outcome, but the check is off: only the digest is read.
    ReviewScript script{MOVE, {"You fly to the lighthouse."},
                        {R"({"consistent":false,"problems":["The player flies."],"digest":"The player went to the lighthouse."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_review_digest_only"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    const auto& calls = backend.Calls(); // resolve, narrate, review
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].spec.grammar == turn::BuildReviewGrammar(false, true));
    CHECK(record->narration == "You fly to the lighthouse.");
    CHECK(record->digest == "The player went to the lighthouse.");
    CHECK_FALSE(record->timings.at("review").at(0).contains("consistent"));
    CHECK_FALSE(record->timings.contains("renarrate"));
}

TEST_CASE("With both switches off no review call is made", "[session][review]")
{
    test::ScriptedBackend backend(16384, Responder(MOVE, "You walk to the lighthouse."));
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, false),
                                                 NewRunFile("session_review_off"), 42, backend);
    const std::optional<core::TurnRecord> record = session.RunTurn("I walk to the lighthouse.", {}, {});
    REQUIRE(record);
    CHECK(backend.Calls().size() == 2); // resolve, narrate
    CHECK_FALSE(record->timings.contains("review"));
    CHECK(record->digest.empty());
}

TEST_CASE("A cancel during the review or the renarrate commits nothing", "[session][review]")
{
    const std::string contradicts = R"({"consistent":false,"problems":["The player flies."],"digest":"The player flew."})";
    ReviewScript script{MOVE, {"You fly to the lighthouse.", "You walk to the lighthouse."}, {contradicts, CONSISTENT}};
    const test::ScriptedBackend::Responder scripted = script.Responder();
    std::stop_source stop;
    turn::TurnCallbacks callbacks;

    SECTION("during the review")
    {
        // The stop arrives as the review call starts, so its reply never streams.
        test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
            if (prompt.messages.front().content == turn::REVIEW_RULES) {
                stop.request_stop();
            }
            return scripted(prompt, spec);
        });
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_review_cancel"), 42, backend);
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
    SECTION("during the renarrate")
    {
        test::ScriptedBackend backend(16384, scripted);
        turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(true, true),
                                                     NewRunFile("session_renarrate_cancel"), 42, backend);
        callbacks.onNarrationRestarted = [&](const std::vector<std::string>&) { stop.request_stop(); };
        CHECK_FALSE(session.RunTurn("I walk to the lighthouse.", callbacks, stop.get_token()));
        CHECK(backend.Calls().size() == 4); // resolve, narrate, review, and the cancelled renarrate.
        CHECK(session.History().empty());
        CHECK(session.State() == MiniWorld().initialState);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `onNarrationRestarted` is not a member of `morpheus::turn::TurnCallbacks`, and `REVIEW_RETRY_OFFSET` is not a member of `morpheus::turn`.

- [ ] **Step 3: Declare the callback and the offset**

In `src/turn/Session.h`, after `constexpr int CONDENSE_SUMMARY_DIVISOR = 2; ...`, add:

```cpp
// Added to a turn's attempt for the seed of its second review, so that seed never equals a regenerated turn's first
// review seed (attempt + 1). PlayTurn requires attempt < REVIEW_RETRY_OFFSET, which keeps the sum within
// SamplingSeed's limit of 2^24.
constexpr int REVIEW_RETRY_OFFSET = 1 << 23;
```

Replace `TurnCallbacks` with:

```cpp
struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    // Every narration's pieces as they are written, the renarrate's included.
    inference::TokenCallback onNarrationToken = {};
    // The review found that the narration contradicts the outcome (`problems` are its reasons): the narration starts
    // over, and the new one streams through onNarrationToken. At most once per turn.
    std::function<void(const std::vector<std::string>& problems)> onNarrationRestarted = {};
};
```

Replace the `RunTurn` comment's first sentence with:

```cpp
    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, (narration_check or
    // compact_transcript: review, and at most one renarrate with its own review), commit. Control characters other
```

(the rest of that comment is unchanged).

- [ ] **Step 4: Review, renarrate and commit**

In `src/turn/Session.cpp`, add `#include "turn/Review.h"` after `#include "turn/ResolveGrammar.h"`.

At the top of `Session::PlayTurn`, before `const int turn = _store.LatestTurn() + 1;`, add:

```cpp
    // A second review seeds with attempt + REVIEW_RETRY_OFFSET, which must stay within SamplingSeed's range.
    MORPHEUS_REQUIRE(attempt >= 0 && attempt < REVIEW_RETRY_OFFSET,
                     "PlayTurn: attempt must be in 0..REVIEW_RETRY_OFFSET-1");
```

Replace everything from the line `    // 4. Narrate: the resolve prompt plus the resolve output and the resolution record, so all of it stays cached.` through the line `    telemetry["narrate"] = Telemetry(narrated, narrateSpec);` with:

```cpp
    // 4. Narrate: the resolve prompt plus the resolve output and the narrate message, so all of the resolve prompt
    // stays cached. `problems` is empty for the first narration; a renarrate passes the review's, which the narrate
    // message shows before its instruction. Both stream through onNarrationToken.
    inference::GenerationSpec narrateSpec;
    narrateSpec.maxTokens = _settings.budget.narrateMaxTokens;
    narrateSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    auto narrate = [&](const std::vector<std::string>& problems, core::CallKind kind) {
        inference::Prompt narratePrompt = assembly->prompt;
        context::ExtendForNarrate(narratePrompt, resolved.text,
                                  RenderNarrateMessage(proposal, resolution, problems,
                                                       _settings.budget.narrateAllowance, *_pBackend));
        inference::GenerationSpec spec = narrateSpec;
        spec.sampling =
            inference::SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, kind, attempt));
        return _pBackend->Complete(narratePrompt, spec, callbacks.onNarrationToken, stop);
    };
    const inference::Completion narrated = narrate({}, core::CallKind::Narrate);
    if (narrated.cancelled) {
        return std::nullopt;
    }
    record.narration = core::Trim(narrated.text);
    telemetry["narrate"] = Telemetry(narrated, narrateSpec);

    // 5. Review, when the narration check or the compact transcript is on: one standalone call reads the outcome
    // and the narration, and says whether they agree (the check), writes the turn's digest (the compact transcript),
    // or both.
    const bool check = _settings.narrationCheck;
    const bool digest = _settings.compactTranscript;
    if (check || digest) {
        inference::GenerationSpec reviewSpec;
        reviewSpec.grammar = BuildReviewGrammar(check, digest);
        reviewSpec.maxTokens = _settings.budget.reviewMaxTokens;
        reviewSpec.thinkingMaxTokens = _settings.budget.reviewThinkingTokens;
        telemetry["review"] = nlohmann::json::array();
        // Reviews `narration` with the sampling seed of `seedAttempt` and adds the call to timings.review; nullopt
        // when cancelled. Unreadable output is a model mistake: its Review holds only the error, so it finds nothing.
        auto review = [&](const std::string& narration, int seedAttempt) -> std::optional<Review> {
            inference::GenerationSpec spec = reviewSpec;
            spec.sampling = inference::SamplingFrom(
                _settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Review, seedAttempt));
            const inference::Completion reviewed =
                _pBackend->Complete(ReviewPrompt(input, proposal, resolution, narration), spec, {}, stop);
            if (reviewed.cancelled) {
                return std::nullopt;
            }
            Review result = ParseReviewOutput(reviewed.text, check, digest);
            nlohmann::json entry = Telemetry(reviewed, spec);
            if (result.consistent) {
                entry["consistent"] = *result.consistent;
            }
            if (!result.problems.empty()) {
                entry["problems"] = result.problems;
            }
            if (!result.digest.empty()) {
                entry["digest"] = result.digest;
            }
            if (!result.error.empty()) {
                entry["error"] = result.error;
            }
            telemetry["review"].push_back(std::move(entry));
            return result;
        };

        std::optional<Review> verdict = review(record.narration, attempt);
        if (!verdict) {
            return std::nullopt;
        }
        if (verdict->consistent == false) {
            // 6. Renarrate, once: the narration contradicts the outcome. The player sees it start over, the new
            // narration streams, and its own review decides the digest. Whatever that review says, the second
            // narration is committed: there is no second renarrate.
            if (callbacks.onNarrationRestarted) {
                callbacks.onNarrationRestarted(verdict->problems);
            }
            const inference::Completion renarrated = narrate(verdict->problems, core::CallKind::Renarrate);
            if (renarrated.cancelled) {
                return std::nullopt;
            }
            telemetry["renarrate"] = Telemetry(renarrated, narrateSpec);
            telemetry["renarrate"]["replaced"] = record.narration;
            record.narration = core::Trim(renarrated.text);
            // The offset keeps this seed apart from a regenerated turn's first review (attempt + 1).
            verdict = review(record.narration, attempt + REVIEW_RETRY_OFFSET);
            if (!verdict) {
                return std::nullopt;
            }
        }
        record.digest = verdict->digest;
    }

    // 7. Commit.
    telemetry["resolve"] = Telemetry(resolved, resolveSpec);
    if (!proposal.error.empty()) {
        // Why nothing was proposed: the record's resolve output alone does not say so for JSON of the wrong shape.
        telemetry["resolve"]["error"] = proposal.error;
    }
```

The lines after it (`telemetry["retrieval"] = ...` through `return record;`) stay as they are.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session]"`
Expected: all pass, the new `[review]` cases included. The older session tests build their settings in code (`TestSettings`), with both switches off, so their call counts are unchanged.

Then: `ctest --preset tests`
Expected: all pass. The app tests copy `settings.example.json` (both switches on), so their turns now make a review call; their responders answer it with narration text, which reads as an unreadable review (a model mistake), so their turns still commit.

- [ ] **Step 6: Commit**

```bash
git add src/turn/Session.h src/turn/Session.cpp tests/turn/SessionTests.cpp
git commit -m "Review each narration and narrate a contradiction once more" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

