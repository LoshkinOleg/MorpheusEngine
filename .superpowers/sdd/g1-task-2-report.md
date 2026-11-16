# G1 Task 2 report
Status: DONE_WITH_CONCERNS
Commits: 9cc493f (comment fixes), then the Task 2 commit (see git log).
Implemented per brief: HostedCallbacks::onSpeaker, RunTurn decide flow, test support (IsHostedDecide, decisions/decided, ScriptOf), tests/hosted/HostedDecideTests.cpp.
Tests: clean build, [decide] 61 assertions / 11 cases pass; ctest --preset tests 683/683 pass (llama/server-live skipped).
Deviations: RED step skipped (support and implementation were written before the first build, so the compile-error RED was not observed). No invalid_requests expectation changed. Line-wrap reflow of comments in the comment commit.

## Fix round
RED: after adding the three tests, `morpheus_tests.exe "[decide]"` failed the events test: "the decide prompt does not fit: its tail uses 1454 tokens, tail_allowance 1024" (budget and no-fit tests already passed).
Fix: decide fit loop drops oldest events while tailOverBudget, then oldest lines while transcriptOverBudget/contextOverflow; TurnError names the tail or the context. Constants moved after HOST_LORE_SOURCE, <climits> removed, comments re-wrapped, RunTurn comment and spec bullet updated.
GREEN: "[decide]" 84 assertions / 14 cases pass; "[hosted]" 1548 assertions / 145 cases pass; ctest --preset tests 686/686 pass.
Note: the context-overflow TurnError message is not tested (not cheaply reachable).
