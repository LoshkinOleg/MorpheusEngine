# Task 5 report
Status: DONE. Implemented per brief: NarrationRestarted event + Worker callback, RunModel clears pending narration, Replay (CallKindOf/Name review+renarrate, digest in TurnResultJson, review settings in MakeReplayFixture), three fixtures hand-edited, morpheus_play rewrite line and fixture call.
TDD: RED was the expected compile failure (NarrationRestarted missing, MakeReplayFixture signature) in the tests preset build; GREEN: `morpheus_tests.exe "[worker],[replay],[run_model]"` all passed (723 assertions, 28 cases); `ctest --preset tests` 269/269 passed.
morpheus_play built clean with build/dev RelWithDebInfo.
Deviations: none (the RED build was not captured as output; I applied tests and sources in one pass before first build).
Concerns: none.
