# E2 Task 6 report

Implemented: two live tests of reload.json (`[llama][hosted][rewind]` in tests/llama/LlamaHostedTests.cpp, `[server][rewind]` in tests/server/ServerLiveTests.cpp; echoedMemories false; modeled on E1's three-actor live tests; they skip without env vars). Docs: README (rewind route row, a "Rewind" paragraph covering the clock, hiding, roll-forward, abandoning; api_version 1.2.0 in two places), architecture spec (3 new tables, version 6, a Rollbacks bullet in Hosted runs), per-speaker memory spec (supersession links via summary_supersessions, rollback can hide memories), server spec (route row, added in 1.2.0 (E2)).

TDD: live tests skip by design; no RED. GREEN: `cmake --build --preset tests` ok; `ctest --preset tests` 100% passed of 647 (live tests skipped, #640 is the new server one); `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests` exit 0.

Byte checks (high bytes / control bytes) on README, all specs, both test files: 0 and 0.

Deviations: per-speaker spec line is about summary_supersessions, as instructed. The architecture spec's `summaries` row text still mentions the `superseded` flag (left; the version-6 note says the table replaces it). Did not edit CHANGELOG (already has 1.2.0). Commit f34c098.

Concerns: README says a no-clock rewind is refused with 400; src throws core::DataError in HostedSession (the Api mapping to 400 was not verified).

## Fix round
Fixed the review's 3 Important and 4 Minor items, docs only: architecture spec rows `summaries`, `conversations`, `pending_shares`, `conversation_changes`, and the version note ("versions 2 to 5 are upgraded in place, others refused", matching RunStore.cpp:541); `clock?` in the open rows (README, server spec); server spec api_version and CHANGELOG sentences; per-speaker spec store section notes; README meaning of rewound/hidden_turns/restored_turns (HostedSession.h:54-56). Byte checks: 0 and 0 on all docs.
