### Task 1: Linux

**Files:** `CMakePresets.json`, `README.md` (Build (Linux)), `tests/server/RunRegistryTests.cpp`, `src/llamaserver/LlamaServerBackend.cpp` (`MakeClient`), `tests/llamaserver/LlamaServerCompleteTests.cpp`.

- [ ] **Step 1:** `CMakePresets.json`: remove `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` from `linux-tests` and `linux-release`. README's Linux section: the configure commands become `CC=clang-20 CXX=clang++-20 cmake --preset linux-tests` (and `linux-release`), with one sentence: the presets name no compiler, so `CC`/`CXX` (any clang 17 or later, versioned names included) or the system default is used.
- [ ] **Step 2:** `tests/server/RunRegistryTests.cpp` (~37): `for (const std::string bad : {std::string(), ...})` becomes `for (const std::string& bad : ...)`. Keep value loops over string literals as they are (no copy).
- [ ] **Step 3:** `src/llamaserver/LlamaServerBackend.cpp`, `MakeClient`: after `set_keep_alive(true)`, `pClient->set_tcp_nodelay(true);` with a comment: each request goes out in small writes (headers, then body); without TCP_NODELAY, Linux holds the second until the peer's delayed ACK, about 40 ms a request.
- [ ] **Step 4:** `tests/llamaserver/LlamaServerCompleteTests.cpp`, "A stop from another thread ends LlamaServerBackend::Complete while the server sends nothing": the stopper thread waits until `chat.RequestsTo("/completion")` is non-empty (polling every 5 ms, up to 5 s), then requests the stop, instead of sleeping 200 ms; the `elapsed < 5 s` check stays. Add a test against the fake server that the backend's requests set `TCP_NODELAY` only if the fake can observe it cheaply; otherwise rely on the Linux run (say which in the report).
- [ ] **Step 5:** `cmake --build --preset tests && ctest --preset tests`. Commit: "Linux: presets take CC/CXX, a loop copy, TCP_NODELAY on llama-server clients, a stop test that waits for the request".

(The controller verifies the Linux build and the fast suite in an Ubuntu container after this task.)

---

