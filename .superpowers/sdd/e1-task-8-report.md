# E1 Task 8 report

Status: DONE. Commit 40ca5e5 "Per-speaker memory: live runs of the three-actor fixture, and the docs".

## Implemented
- tests/hosted/HostedTestSupport.h: `ScriptOf(fixture)` helper; `PlayMemoryFixture(name, backend, settings, echoedMemories)`; the one-argument form is a wrapper over the echo backend. With echoedMemories false: skips prompt_contains, memories contains, told_by, a share's `shared`; checks prompt_absent, memories absent/count, share pending, that each turn commits.
- tests/server/ServerTestSupport.h: `PlayMemoryFixtureOverHttp(client, runId, fixture, echoedMemories, limit)` extracted from HttpMemoryTests. Its memories step now polls (WaitUntil) until the actor has the expected memories (all `contains` texts when echoed; at least one memory and `count` otherwise) before the absent/count/told_by checks, so they no longer race the background fold.
- tests/server/HttpMemoryTests.cpp: now uses the helper and ScriptOf; unused `<vector>`/`<string>`/ContainsSubstring removed.
- tests/llama/LlamaHostedTests.cpp: `[llama][hosted][memory]` test (skips without the model variables).
- tests/server/ServerLiveTests.cpp: `[server][memory]` test over HTTP on LlamaServerBackends, echoedMemories false, 5 minute wait limit.
- CHANGELOG: API 1.1.0 now says "Two new routes; and a change of behaviour: every NPC now remembers only what it witnessed."; last bullet reworded.
- tests/turn/ReplayTests.cpp: stale "As settings.example.json" comments now say the budget the replays were recorded with; no value changed.
- Docs (ASCII, byte check 0/0 on every changed file): README (api_version 1.1.0, two routes in the table, memory paragraph, live-test sentence), architecture spec (turn row, summaries columns, actor_memory and pending_shares rows, schema version 5 with in-place upgrade of version 4, hosted prompt: memories in the tail, L2 empty, MemoryUpkeep through a SummaryScope), hosted-world spec (three one-line E1 pointers), morpheus_server spec (two routes marked added in 1.1.0 (E1), changelog line). CLAUDE.md unchanged (no layout line changed).

## Evidence
- No RED step: this task adds live tests and docs, the fast-suite behaviour is unchanged. The refactored echo and HTTP fixture tests are the regression check.
- `cmake --build --preset tests` clean; `./build/tests/bin/Debug/morpheus_tests.exe "[memory]"`: 27 passed, 1 skipped (297 assertions).
- `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`: compiled with no errors (the [llama] file builds).
- `ctest --preset tests`: 100% tests passed out of 603; [llama]/[server] tests skipped (variables unset).
- Byte check (non-ASCII, control bytes): 0 and 0 for all 11 changed files.

## Deviations
- The llama live test overrides the budget and sampling as the existing hosted [llama] test does ({96,160,128,1024,1024,1024,512,256}, greedy) instead of HostedSettings' budget, because the shared backend's context is 4096 and HostedSettings' budgets sum to about 4000+ plus the prompt. The [server] test uses HostedSettings with LIVE_CONTEXT_SIZE as the existing live test does.
- The HTTP helper takes a `runId` and a wait `limit` (default 10 s) parameters; the live test passes 5 minutes.

## Concerns
- The live tests were not run (no model variables). A real model that writes a memory containing "takes bribes" or "back to his shop" unprompted would fail an absent check; the failure names the step via INFO.
- Git warns that LF will become CRLF on the touched files (autocrlf); pre-existing.
