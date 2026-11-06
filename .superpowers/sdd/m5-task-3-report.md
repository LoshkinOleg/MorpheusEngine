# Task 3 report
Implemented per brief: BuildReviewGrammar, review constants, REVIEW_RULES/REVIEW_INSTRUCTION/RENARRATE_CORRECTION, OutcomeDetail/RenderOutcome, RenderNarrateMessage with problems (Session.cpp caller updated), turn/Review.{h,cpp}, tests.
RED: `cmake --build --preset tests` -> C2660 "RenderNarrateMessage does not take 5 arguments" (after updating existing calls with sed).
GREEN: `morpheus_tests.exe "[grammar],[resolve],[review]"` -> all passed (197 assertions, 28 cases); `ctest --preset tests` -> 100% passed out of 257.
Deviation: in ReviewTests.cpp, `content == std::string(turn::REVIEW_RULES)` (Catch's string_view StringMaker failed to link otherwise).
Files: as in the brief's commit step.
Concerns: none.

## Fix round
Changes: unreadable-proposal early return removed from RenderNarrateMessage (header comment updated); new test for it; intermediate-allowance case in the "shed last" test; ParseReviewOutput enforces REVIEW_MAX_PROBLEMS, REVIEW_PROBLEM_MAX_CHARS, DIGEST_MAX_CHARS and rejects empty-after-trim problems; grammar problem-text is char{1,200}; table rows added (Case::text is now std::string).
RED: not observed as a separate run. I applied tests and implementation together, so only the GREEN run is evidence (a process lapse).
GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[grammar],[resolve],[review]"` -> All tests passed (223 assertions in 29 test cases); `ctest --preset tests` -> see commit run below.
100% tests passed out of 258

### Fix round 2 (UTF-8 lengths)
Added core::Utf8Length (code points) with a "[text]" test; ParseReviewOutput counts problems and digest in characters, with bounds in the reasons ("more than 3 contradictions", "longer than 200 characters", "longer than 300 characters"). New test case "ParseReviewOutput bounds problems and the digest in characters, not bytes" (150 and 300 two-byte chars accepted, 201/301 rejected) instead of table rows.
RED: `cmake --build --preset tests` -> C2039/C3861 'Utf8Length' is not a member of 'morpheus::core'.
GREEN: `morpheus_tests.exe "[review],[text]"` -> All tests passed (100 assertions in 9 test cases); `ctest --preset tests` -> 100% passed out of 260.
