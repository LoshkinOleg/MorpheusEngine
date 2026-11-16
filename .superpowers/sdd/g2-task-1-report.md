# G2 Task 1 report: min_p
Implemented min_p per brief (settings, Sampling, SamplingFrom, in-process chain, llama-server body with old neutral "min_p": 0.0 removed, SettingsEdit, SettingsWindow, hosted test settings, example, README).
RED: `cmake --build --preset tests` -> C2039 'minP' is not a member of SamplingSettings.
GREEN: `cmake --build --preset tests --clean-first` + `ctest --preset tests`: 100% passed out of 696. `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`: LlamaCppBackend.cpp compiled, no errors.
Non-ASCII/control-byte checks on added lines: 0 in all files.
Deviations: none material; README line added as a bullet after the Qwen sampling line; server comment re-wrapped.
Concerns: none. dev tests were built, not run.
