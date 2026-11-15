# Task 11 report
Status: DONE. Commit f740571.
Implemented tools/server/{CMakeLists.txt,main.cpp} and tests/server/ServerLiveTests.cpp verbatim from the brief; CMakeLists.txt adds tools/server.
Checks: `[server]` run: 4 test cases, 4 skipped (no env vars). tests preset builds clean; dev preset (RelWithDebInfo) builds morpheus_server clean. ctest --preset tests: all pass, live tests skipped.
Exit codes: `--settings nowhere.json` -> "morpheus_server: cannot read 'nowhere.json'", exit 1. `--port 0` -> port message + usage, exit 2. Closed-port settings (build/tests/server-check/closed-port.json, untracked) -> "morpheus_server: cannot reach http://127.0.0.1:1/props: Could not establish connection", exit 1.
Skipped: live /health check (no llama-server running; settings.json names llama-cpp, untouched).
ASCII/control-byte greps: 0 and 0 for both files. No processes left running.
Deviations: none. Concerns: none.
