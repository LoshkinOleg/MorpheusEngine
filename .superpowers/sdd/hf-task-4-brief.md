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
