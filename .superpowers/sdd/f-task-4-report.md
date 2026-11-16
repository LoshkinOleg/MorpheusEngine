# Task 4 report: better default texts

Status: DONE. Commit a867506. Full suite green: `ctest --preset tests` reports 100% of 665 tests passed (the real-server tests skip as designed).

## Implemented
- src/turn/EngineText.h
  - HOSTED_SPEAK_INSTRUCTION is now "Step 2: write what {speaker} says now. Spoken words only, in character." (the `{player}` sentence is removed).
  - HOSTED_SYSTEM_TEXT "Step 1, act." paragraph gains " Most turns need no action: when the speaker would only talk, choose the action that does nothing, if the turn allows one." after "Choose what the speaker would do now.". Nothing else in the raw string changed.
- tests/turn/PromptPackTests.cpp: `#include <string_view>`; the brief's new [prompts] case. Deviation: both comparisons wrap views in std::string(...) and use std::string::npos (the brief's noted fallback; the first link failed on Catch::StringMaker<std::string_view>).
- tests/hosted/HostedTurnTests.cpp: speak expectation is "Step 2: write what Hulda says now. Spoken words only, in character."
- tests/context/HostedPromptTests.cpp: removed the `{player}` check on HOSTED_SPEAK_INSTRUCTION; the `{speaker}` check and the brace loop remain.

## TDD evidence
- RED (link time): the first build with the new test failed with LNK2019 on Catch::StringMaker<std::string_view>::convert. Fixed by the std::string wrap above.
- RED (behaviour): with the old engine text in place, the test binary built from the wrapped test showed the expected failures before the header restore: the speak expectation (HostedTurnTests line 110) and the PromptPackTests speak and system checks. That binary was then stale (see Build notes), so the run that shows the failures is the one with the old text in the header.
- GREEN: `cmake --build --preset tests` exit 0. `./build/tests/bin/Debug/morpheus_tests.exe "[prompts],[hosted][turn]"`: "All tests passed (253 assertions in 26 test cases)". `ctest --preset tests`: "100% tests passed out of 665".

## Build notes
- A hung build from an earlier background run held the test PDB (C1041) until it finished; the coordinator confirmed no cl.exe or MSBuild was left running before the rebuild.
- After the header was restored, MSBuild judged the test objects up to date (they had been compiled from the old header during the hung build), so the first GREEN attempt ran the stale binary. Touching src/turn/EngineText.h (no content change) forced the dependents to recompile; the rerun passed.

## Files committed (a867506)
- src/turn/EngineText.h
- tests/turn/PromptPackTests.cpp
- tests/context/HostedPromptTests.cpp
- tests/hosted/HostedTurnTests.cpp

## Byte checks (committed files)
- LC_ALL=C grep -c $'[\x80-\xFF]': 0 for all four files
- LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]': 0 for all four files

## Concerns
- None blocking. The real-server `[server]` and llama-server tests were skipped (they need running llama-servers).
