# Task 1 report
Implemented the three texts, PromptPack::review (8th text), HostedReviewPrompt, per brief verbatim.
RED: cmake --build --preset tests -> C2039 'review' is not a member of PromptPack, HOSTED_REVIEW_RULES undeclared, HostedReviewPrompt not a member.
GREEN: clean-first build, no warnings; "[review],[prompts]": All tests passed (355 assertions in 34 test cases); ctest --preset tests: 100% passed out of 709.
ASCII/control-byte greps: 0 0 for all 8 files. Escape sequences: a first edit decoded \n in the Review.h comment/Review.cpp; redone with chr(92); final files show literal \n.
Deviations: none (ApiPromptTests api_version stays 1.4.0; the bump is a later task).
