# Task 7 report: backend keys in settings.json

Commit: 5d21f6b "settings.json chooses the backend: llama-cpp or llama-server with its server URLs and slot"

## Implemented
- `core::BackendKind`, `ParseBackendKind`, `BackendKindName`; `Settings` fields `backend`, `chatUrl`, `embeddingUrl`, `serverSlot`.
- `LoadSettings`: required `backend`; llama-cpp requires the model keys, llama-server requires a `server` object (chat_url, embedding_url "http://" plus host, slot >= 0); the other backend's keys are ignored.
- `SettingsToJson` writes `backend` and only the keys of the backend in use.
- `settings.example.json` gains `"backend": "llama-cpp"` as its first key.
- This machine's gitignored `settings.json` got `"backend": "llama-cpp"` as the first key (not staged; `git status --short` does not list it).

## TDD evidence
- RED: `cmake --build --preset tests` failed in tests/core/SettingsTests.cpp: `C2039: 'backend': is not a member of 'morpheus::core::Settings'`, `C2653: 'BackendKind': is not a class or namespace name`.
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[settings]"`: All tests passed (172 assertions in 12 test cases).
- `ctest --preset tests`: 100% tests passed out of 417 (414 + 3 new test cases).
- `ctest --preset asan` (after `cmake --build --preset asan`): 100% tests passed out of 417 (221 s). Only warning: LNK4300 /INCREMENTAL ignored under ASAN (pre-existing, linker notice).
- dev: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe` succeeded.
- `morpheus_probe.exe settings.json game_projects/sandcrawler "I look around."` (models present): streamed two completions; run 2 reused 718 of 719 prompt tokens.

## Byte checks
- `check_bytes` on the four files: no output.
- `LC_ALL=C grep -n $'[\x80-\xFF]'` on each: no output. Control-byte count `grep -c $'[\x01-\x08]'`: 0 for each file (the brief's `[\x00-...]` range form is rejected by this grep; the check_bytes perl covers all control bytes except tab, LF, CR).
- No `\u`/`\x` escapes are in this change, so no escape greps were needed.

## Files changed
src/core/Settings.h, src/core/Settings.cpp, settings.example.json, tests/core/SettingsTests.cpp (CRLF line endings kept in the C++ files).

## Deviations
- Per the controller: test and example URLs use 127.0.0.1 (the brief's `http://localhost:8081` became `http://127.0.0.1:8081`); Settings.cpp comment reads "a local host such as 127.0.0.1 is the intended use".
- Edits were made with a Python script that preserves CRLF; text is verbatim from the brief otherwise.

## Concerns
- I started a second, redundant `ctest --preset asan` by mistake and stopped it; nothing is left running.
