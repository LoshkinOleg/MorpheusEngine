
# Final fix round report

Changes: HostedInputs::reviewReserve (src/context/PromptBuilder.h); AssembleHostedPrompt adds it to the narrate
allowance and passes it to LayOut, whose no-room DataError appends "(the narrate allowance includes the review's
reserve of N tokens: the review message, review_max_tokens, review_thinking_tokens and the chat-template margin)"
only when N > 0 (unreviewed message unchanged). HostedSession::Pass sets inputs.reviewReserve instead of editing the
budget. New test "A reviewed turn with no transcript room names the review's reserve" (HostedReviewTests.cpp), written
alongside the code (not run RED separately). Stale comments (Settings.h/.cpp), HostData::prompts comment, the four doc
edits, CHANGELOG knows clause, README Linux status, CMake clang/GCC ImGui warning flags, long lines wrapped (added
lines only; EngineText.h raw strings untouched; pre-existing long lines elsewhere left), REVIEW_TEMPLATE_MARGIN comment.
Note: the "~625" literal 16 was at HostedReviewTests.cpp:207 (file has 408 lines).

Commands/results: cmake --build --preset tests --clean-first (ok); morpheus_tests "[review]" 316 assertions/29 cases
passed; "[hosted]" 1855 assertions/174 cases passed (4 reruns; one earlier run failed only because it overlapped a
second concurrent test run sharing temp dirs); ctest --preset tests: 100% passed out of 746; cmake --build build/dev
--config RelWithDebInfo: exit 0, no warnings. ASCII/control-byte greps: all 0. "\n\n" still literal in PromptBuilder.cpp.
