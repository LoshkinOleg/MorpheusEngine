# Task 1 report: Linux
Implemented all five steps: presets drop CMAKE_C/CXX_COMPILER; README uses CC=clang-20 CXX=clang++-20 plus one sentence; RunRegistryTests line 37 loop takes `const std::string&`; MakeClient calls set_tcp_nodelay(true) with comment; stop test polls chat.RequestsTo("/completion") every 5 ms up to 5 s before stopping (also added CHECK that /completion was received).
TDD: no RED possible on Windows (the race only shows on Linux; the controller's Linux run is the evidence). TCP_NODELAY is not observable cheaply in the fake: no test added, relies on the Linux run.
GREEN: cmake --build --preset tests; ctest --preset tests: 100% passed out of 728.
Non-ASCII/control greps on the 3 C++ files: all 0.
Line endings: files are LF in the index and working copy here (git warns about CRLF conversion only); a sed-inserted CR in README was removed.
Commit 85233c2. Files: CMakePresets.json, README.md, tests/server/RunRegistryTests.cpp, src/llamaserver/LlamaServerBackend.cpp, tests/llamaserver/LlamaServerCompleteTests.cpp.
