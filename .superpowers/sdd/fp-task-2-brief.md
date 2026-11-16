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

