# Task 3 report
Implemented per brief: HostedSession reads system/resolve/speak/turn_cue/summarize/condense from _world.Prompts(); Open counts the system text before touching the file and throws "prompts/system: <n> tokens, over engine_rules_cap <cap>". Removed SPEAKER/PLAYER/ACTOR_VARIABLE and FillVariables. Header comments updated.
RED: `morpheus_tests.exe "[hosted][prompts]"`: 6 cases, 1 passed, 5 failed (default texts in prompts, no over-cap error).
GREEN: `"[hosted]"`: all passed (128 cases, 1449 assertions); `ctest --preset tests`: 100% passed of 664 (6 [llama]/[server] skipped).
Files: src/turn/HostedSession.h, src/turn/HostedSession.cpp, tests/hosted/HostedPromptPackTests.cpp.
Deviations: none. No existing test pinned the requests open makes; none needed updating. EngineText.h include kept (still used for HOSTED_WORLD_ID etc.).
ASCII/control-byte greps: 0 and 0 for all three files. Concerns: none.
