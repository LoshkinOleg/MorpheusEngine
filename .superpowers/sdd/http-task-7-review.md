### Spec Compliance
- Spec compliant. Every item of the brief is in the diff: BackendKind, ParseBackendKind, BackendKindName (Settings.h, Settings.cpp:62-82); the Settings fields; `backend` required with the named messages; llama-cpp needs the model keys (Settings.cpp:142-145); llama-server needs `server` with chat_url, embedding_url ("http://" plus a host) and slot 0..INT_MAX, unknown keys in `server` rejected via checkKeys; the other backend's keys ignored; SettingsToJson writes `backend` and only the active backend's keys; settings.example.json gains `backend` first; all required tests are present; the 127.0.0.1 changes are applied (tests and Settings.cpp:147 comment).
- Cannot verify from diff: this machine's gitignored settings.json edit and the unstaged status (report claims only); the exact text produced by requireMember/requireInt for the "needs 'server'" and slot-range messages (tests assert only that the key name is contained, and the report says they pass).

### Strengths
- Tests cover each failure mode with the key named, the both-ways "ignored" cases (including values that would not load), the SettingsToJson round trip per backend, and the absence of the other backend's keys.
- Validation fails fast and loudly with file-naming DataErrors; BackendKindName throws ContractError on an unhandled enum; no fallback paths.
- Model-key reads moved inside the llama-cpp branch, so nothing is read and then ignored; comments explain intent.
- Cross-cutting check: src/app/model/SettingsEdit.cpp:45-48 and :80-83 still read/write chat_model, embedding_model and gpu_layers unconditionally, so the Settings window would fail on a llama-server file. The plan assigns this to a later task (plan line 4601-4610), so it is not a defect of Task 7.

### Issues
#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
1. src/core/Settings.cpp:153: the URL check accepts "http://" followed by no real host, e.g. "http:///x" or "http://:8080", and any trailing path or whitespace; the message promises "name a host". Brief-mandated check, and the later URL validation in the backend (Task 1 of the series) is likely the stronger gate; consider noting it.
2. src/core/Settings.cpp:223: the `json["server"] = {...}` line is about 140 columns, longer than the file's other lines (about 120); formatting only (brief-verbatim).
3. tests/core/SettingsTests.cpp: no test that `"slot"` of a non-integer type (e.g. "1") or above INT_MAX is refused; only -1 is covered. The range logic is in the shared requireInt, so low risk.

### Assessment
**Task quality:** Approved
**Reasoning:** The implementation matches the brief point for point with fail-fast validation and meaningful tests; the only gaps are polish-level. SettingsEdit's unconditional model keys are a known later task.
