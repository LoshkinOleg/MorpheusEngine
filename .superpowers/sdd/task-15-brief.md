### Task 15: AddressSanitizer over the llama.cpp backend

**Files:**
- Modify: `CMakePresets.json`, `README.md`, `CLAUDE.md` (the presets line, if it lists them)
- Modify as findings require: `src/llama/*`, `src/turn/*`, with regression tests

**Interfaces:**
- Consumes: everything.
- Produces: the `dev-asan` configure, build and test presets.

- [ ] **Step 1: Add the preset**

In `CMakePresets.json`, add a configure preset:

```json
    {
      "name": "dev-asan",
      "displayName": "llama.cpp on the CPU + tests under AddressSanitizer",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/dev-asan",
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_VULKAN": "OFF",
        "MORPHEUS_ASAN": "ON",
        "GGML_OPENMP": "OFF"
      }
    }
```

Add a build preset `{ "name": "dev-asan", "configurePreset": "dev-asan", "configuration": "RelWithDebInfo" }`. Add a test preset with the same name and configuration, and with `output` and `execution` as the other test presets.

Choices behind the preset:
- **RelWithDebInfo:** a Debug, unoptimized ggml is too slow on the CPU for a 4B model. ASan works on optimized MSVC builds.
- **No Vulkan:** ASan instruments CPU code only. GPU kernels would hide the backend's memory traffic.
- **No OpenMP:** it is off to keep MSVC's OpenMP runtime out of the sanitized build.

- [ ] **Step 2: Build and run under ASan**

Run each command in the background or with the max timeout. The first build compiles all of llama.cpp.

```bash
cmake --preset dev-asan
cmake --build --preset dev-asan
ctest --preset dev-asan
```

Then, with the model variables exported:

```bash
./build/dev-asan/bin/RelWithDebInfo/morpheus_tests.exe "[llama]"
./build/dev-asan/bin/RelWithDebInfo/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."
printf 'I check the fuel gauge.\n/undo\n/quit\n' | ./build/dev-asan/bin/RelWithDebInfo/morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/asan.db --seed 5
```

Expected:
- No AddressSanitizer report.
- The tests pass, and the probe and play run (slowly, on the CPU).

If the build fails inside llama.cpp or ggml under `/fsanitize=address`, stop. Report the error as DONE_WITH_CONCERNS, without patching fetched code.

- [ ] **Step 3: Fix findings**

For each ASan report in MorpheusEngine code (`src/llama`, `src/turn`, anything ours):
1. Write a test that reproduces it under `dev-asan` (it may be a `[llama]` test).
2. Fix it.
3. Show the report gone.

A report inside llama.cpp or ggml itself goes in the task report with its stack. Do not patch it.

- [ ] **Step 4: Document and commit**

`README.md` presets table, new row: `dev-asan`: "llama.cpp on the CPU (no Vulkan) and the tests under AddressSanitizer, RelWithDebInfo; slow, for memory checks of the backend". Mirror it in `CLAUDE.md` if it lists the presets.

```bash
git add CMakePresets.json README.md CLAUDE.md src tests
git commit -m "Add the dev-asan preset and run the llama backend under AddressSanitizer" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

## After the last task

- Run a final whole-branch review over the milestone (`git merge-base` with the M2 end, `bf017b7`, through HEAD). Point it at the Minor findings recorded in `.superpowers/sdd/progress.md`.
- Once it is clean, the owner plays with `morpheus_play`. Tuning `RETRIEVAL_MIN_SCORE`, `NAME_BOOST` and the instruction texts from that play is the bridge into M4. M4 is the ImGui app over `turn::Worker`, its context inspector reading `TurnCompleted::assembly`, and its turn log reading the record's telemetry.
