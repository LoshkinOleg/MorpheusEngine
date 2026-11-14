# Task 8 report
Implemented per brief verbatim: SettingsEdit backend/chatUrl/embeddingUrl/serverSlot (read/save), SettingsWindow backend combo with llama-server fields.
RED: `cmake --build --preset tests` failed: C2039 'backend' / 'chatUrl' / 'embeddingUrl' / 'serverSlot' is not a member of SettingsEdit (SettingsEditTests.cpp).
GREEN: `[settings_edit],[ui]` all passed (376 assertions, 24 cases); `ctest --preset tests` 100% of 421 (417+4); `ctest --preset asan` 100% of 421; dev targets morpheus, morpheus_play, morpheus_probe built (RelWithDebInfo). No frame run (brief gives none).
Byte checks: non-ASCII grep and control-byte grep print nothing/0 for all 5 files; no 'localhost' in them.
Deviations: files are CRLF (except UiTests.cpp, LF); edits preserved each file's endings. Control-byte grep used [\x01-\x08\x0b\x0c\x0e-\x1f] since the instruction's range is invalid in this grep.
Concerns: none.
