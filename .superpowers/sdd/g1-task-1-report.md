# G1 Task 1 report (written by the controller)

The implementer (Sonnet) made every change in the brief, then was stopped by the controller before it reported:
its test runs hit an MSVC debug assertion ("null pointer cannot point to a block of non-zero size", xmemory 971),
one modal dialog per ctest process.

Diagnosis (controller): stale object files. An incremental build left objects compiled against the old PromptPack
layout (six strings; Task 1 adds `decide`), so an existing F test, "A speak text over narrate_allowance is a DataError
naming prompts/speak", crashed with SIGSEGV. `cmake --build --preset tests --clean-first` fixed it; no code change.

Verification (controller, after the clean rebuild, on the implementer's working tree):
- `./build/tests/bin/Debug/morpheus_tests.exe "[prompts]"`: All tests passed (160 assertions in 19 test cases).
- `ctest --preset tests`: 100% tests passed out of 674 (the [server]/[llama] cases skipped as usual).
- ASCII and control-byte greps: 0 on all 11 changed files.

No RED run was recorded. Deviations from the brief: none found in the diff. Committed by the controller as the
implementer's work.
