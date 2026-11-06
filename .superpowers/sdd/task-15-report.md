# Task 15 report: dev-asan preset

Status: DONE. No AddressSanitizer report from any run, so no source fix and no regression test were needed.

## Implemented
- CMakePresets.json: configure preset `dev-asan` (verbatim from the brief), build preset and test preset (RelWithDebInfo, output/execution like the others).
- README.md presets table row and CLAUDE.md build section line (with the binary folder).

## Evidence
- `cmake --preset dev-asan`: OK (110 s). CPU backend variant ggml-cpu /arch:AVX2, no Vulkan, no OpenMP.
- `cmake --build --preset dev-asan`: exit 0. llama.cpp and ggml compiled under /fsanitize=address with no error. Only third-party warnings (C4244 in MSVC headers via llama.cpp, C5105 in winbase.h) and LNK4300 (incremental link ignored because of ASan metadata). Built morpheus_tests, morpheus_probe, morpheus_play, and both clang_rt.asan DLLs were copied next to them.
- `ctest --preset dev-asan`: 100% passed, 211 tests, 30 s (the model-backed ones are Skipped without env vars).
- `[llama]` tests with both model env vars exported: "All tests passed (86 assertions in 20 test cases)", real 8m15s. 0 ASan reports. (The line "parse: error parsing grammar ... failed to parse grammar" in the log comes from the test that deliberately feeds a bad grammar.)
- morpheus_probe (settings.json, sandcrawler, "I climb onto the crawler's roof."): exit 0, 8m01s. Prompt 723 tokens; run 1 evaluated 723 tokens in 303 s, run 2 reused 722 of 723 (1 evaluated in 785 ms), so KV-cache reuse works. Two narrations produced. 0 ASan reports.
- morpheus_play (saves/playtest/asan.db removed first, --seed 5, input: a fuel gauge turn, /undo, /quit): exit 0, 18m51s. One turn played (resolve 77 tok in 64.9 s, narrate 103 tok in 85.3 s, prompt 1493 tok), the change `player.location = "open desert"; player.health = 10` was applied, `/undo` printed "Back to turn 0.", clean quit. 0 ASan reports.

## Files changed
CMakePresets.json, README.md, CLAUDE.md.

## Deviations
None. For the probe/play run I had accidentally set MORPHEUS_TEST_CHAT_MODEL=x in the shell; those tools read settings.json and do not use it.

## Concerns
- ASan coverage covers only instrumented MorpheusEngine code plus llama.cpp/ggml compiled with /fsanitize=address; ggml's CPU kernels use the heap allocator, so heap errors would be caught, but the run is a single short scenario.
- Runs are slow (minutes per call on the CPU).
