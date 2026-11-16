# Task 4 report
Implemented: LatestClock unions the game_time of the most recent non-undone rollback (RunStore.cpp/.h, comments); HostedSession::Rewind returns early before SyncHistory when rewound is false (cpp/h comments); rollback spec section 1 and 3 updated.
TDD: RED `morpheus_tests "[rollback],[rewind]"`: 4 failed (LatestClock 2.5 not 2.7/2.2; event clock 2.0 not 2.5; hosted store 1.0 not 1.5). GREEN after the change: `[rollback],[rewind],[hosted]` 182 cases, 181 passed, 1 skipped; `ctest --preset tests` 100% passed of 723.
Tests: new store case (open, undone, abandoned then newer rollback), new hosted cases (event dated 2.5 after rewind; no-op rewind makes no backend/embedding call; the latter passed before the fix too, since SyncHistory embeds nothing new when unchanged, so it is a guard).
Updated old pins: RollbackStoreTests line 125 (2.5 -> 2.7); HostedStoreTests "latest clock skips hidden rows" last check (1.0 -> 1.5, the open rollback's time). Reload fixture and README pin nothing and pass unchanged.
Files: src/store/RunStore.{h,cpp}, src/turn/HostedSession.{h,cpp}, the rollback spec, tests/store/RollbackStoreTests.cpp, tests/store/HostedStoreTests.cpp, tests/hosted/HostedRewindTests.cpp.
Deviation: abandon scenario uses 2.2/2.1 on the file's Run helper (visible records reach 4.5) instead of 2.5/2.4. ASCII greps: 0 and 0 on every changed source/test file.
Concern: a stale background build/ctest from a harness timeout held the PDB/exe for a while; killed, rebuilt cleanly.
