# Task 3 report
Commits: first the review-findings commit (one core::ReadStringArray used by ReadActors and HostDataFromJson; HostedRecord.h comment re-wrapped; leading-zero pointer test), then the Task 3 commit.
Implemented: MemoryIndex::Retrieve loreVisible filter (last param); HostedWorld::AlwaysOnLore(observation); HostedSession builds the speaker/observation predicate for retrieval, decide prompt and Pass (resolve and fold) use AlwaysOnLore; API 1.6.0 (Api.h, tests/server pins, README, CHANGELOG, fixtures README, hosted-world and server specs).
Tests: RetrievalTests (filter), tests/hosted/HostedLoreTests.cpp (scoped retrieval Hulda/Saadia, system message stage 30/40, decide prompt, fold prompt). GREEN: "[lore],[retrieval]" all passed; ctest --preset tests 100% of 742 (skips are live tests).
TDD note: implementation was written before the first test run (RED not captured separately); tests first failed only on schema issues in the test data (quests key), fixed by adding quests to the test's observation schema.
Deviations: AlwaysOnLore keeps only always-on entries as a HostedWorld method (two callers). Server spec api_version line updated to 1.6.0. Byte greps on changed sources: 0 / 0.
Concerns: none.

## Fix round
- Test: retrieved entry "Ledger" requires /quests/MG03 min 40 (HostedLoreTests). RED: with the condition check in the predicate replaced by `if (false)`, `morpheus_tests "[lore]"` failed (CHECK_FALSE Ledger at stage 30: !true; 13/14 cases pass). GREEN after restoring: all pass.
- HostedWorld::LoreVisible (static) and VisibleSubjects (a set, built once per turn); AlwaysOnLore and the retrieval predicate share the rule. The "cannot happen" fallback is gone (a set lookup; the index holds only host retrieved lore).
- Docs/style: README world_cap sentence, L1 row, CHANGELOG wording, fixtures README wrap, HostedRecord.cpp brace, RetrievalTests include order.
- Commands: `cmake --build --preset tests --clean-first`; `morpheus_tests "[lore],[retrieval],[hosted]"`: All tests passed (1931 assertions in 189 test cases); `ctest --preset tests`: 100% tests passed out of 743. ASCII/control greps: 0/0 on changed sources.
