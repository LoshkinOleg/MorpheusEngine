# Task 9 review

### Spec Compliance
- Spec compliant. EngineHost.cpp:20-26 adds backend kind, chatUrl, embeddingUrl, serverSlot to sameBackend; BackendFactoryByKind (EngineHost.cpp:101-115, header comment updated) requires both factories and switches on kind; main.cpp, play and probe build either backend; CMake links added; tests match the brief. No `localhost` anywhere in the diff (grep). Settings-window promise is now true.
- Module rule holds: llamaserver header only forward-declares httplib::Client (LlamaServerBackend.h:14-15), httplib is a PRIVATE link of morpheus_llamaserver (llamaserver/CMakeLists.txt:8); the diff adds no <httplib.h> or <llama.h> include to the app/tools.
- Cannot verify from diff: dev-preset build, smoke runs (report evidence only).

### Strengths
- Tests drive the real behavior: each changed field forces a rebuild (EngineHostTests.cpp new case), the same servers keep the backend, and the controller test goes through ApplySettings to the right factory.
- Fail-fast: empty factories throw ContractError; an unhandled enum throws instead of falling back.
- 127.0.0.1 used throughout; probe prints the server it targets.

### Issues
#### Critical (Must Fix)
None.
#### Important (Should Fix)
None.
#### Minor (Nice to Have)
1. The LlamaServerConfig-from-Settings block is written three times (src/app/main.cpp MakeServerBackend, tools/play/main.cpp ~430-437, tools/probe/main.cpp ~525-532), and the LlamaConfig block likewise (as before). Brief-mandated; a shared helper would need a home that links both backends.
2. sameBackend compares all fields regardless of kind, so a chatUrl edit under llama-cpp (or a model-path edit under llama-server) reloads needlessly. Harmless, conservative, and as specified.
3. tools/play does not print the server line the probe prints; asymmetric but unrequested.

### Assessment
**Task quality:** Approved
**Reasoning:** The diff implements the brief exactly, the rebuild condition and the factory chooser are tested for each trigger, and the module and 127.0.0.1 rules hold. Only minor duplication remains.
