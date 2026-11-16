### Task 5: Leftovers from the G1, G2 and F reviews

**Files:** as each item says. Tests first where there is behaviour.

- [ ] **Step 1: Call-tag constants.** Create `src/turn/CallTag.h` with `inline constexpr std::string_view` constants `LOOKUP_TAG = "lookup"`, `RESOLVE_TAG`, `NARRATE_TAG`, `RENARRATE_TAG`, `REVIEW_TAG`, `SUMMARIZE_TAG`, `CONDENSE_TAG`, `DECIDE_TAG`, `SPEAK_TAG`, `RESPEAK_TAG` (a comment: the call tags the turn layer sets, generation-controls spec section 2). Use them where the tags are set (`Session.cpp`, `HostedSession.cpp`, `MemoryUpkeep.cpp`, as `std::string(...)`) and where they are read (`Replay.cpp`'s `CallKindOf`, `app/model/ModelSeat.cpp`'s `SeatLabelOf`, `tests/hosted/HostedTestSupport.h`). Tests keep their literals (they pin the spelling).
- [ ] **Step 2: `thinking_format` error for a non-string, non-object value.** `src/core/Settings.cpp`: a `thinking_format` that is neither a string nor an object (a number, null, an array, a boolean) gets the preset message (`'thinking_format' must be "auto", "qwen", "none" or an object with open, skip, end, after_end and close, got <value as JSON>`), not "must be an object". Test in `tests/core/SettingsTests.cpp` with `5` and `null`.
- [ ] **Step 3: The settings editor keeps `thinking_format`.** `tests/app/SettingsEditTests.cpp`: a settings file with `"thinking_format": "qwen"`, an edit saved, the file re-read still has `"thinking_format": "qwen"`.
- [ ] **Step 4: Empty-text guards in the llama-server backend.** `src/llamaserver/LlamaServerBackend.cpp`: guard an empty `afterEnd` before tokenizing, as `skip` is guarded (an empty text tokenizes to no tokens without a request). Test against the fake server: a custom format with an empty `after_end` (and a `close` ending with its `end`) loads and makes no `/tokenize` request for an empty text (check the requests' bodies).
- [ ] **Step 5: Comments and docs.**
  - `src/turn/HostedSession.h`, `RunTurn`'s comment: "no events" (the decide rule) becomes "no new events".
  - `tests/turn/SamplingTests.cpp`: the SamplingFrom min_p case's tag `[sampling]` becomes `[turn]`.
  - `README.md`: the budget bullet that defines a thinking model as one whose template has `enable_thinking` says that is the `auto` rule and points to `thinking_format`; one sentence in the `thinking_format` description: a custom format on a template that has the enable_thinking switch is used over llama-server with the template's own thinking switch on (prefer `qwen` or `auto` for Qwen models).
  - Re-wrap to about 120 columns: `tests/hosted/HostedPromptPackTests.cpp` (~30), `tests/hosted/HostedWorldTests.cpp` (the two ~125-column lines), `src/context/PromptBuilder.h` (any line over 120), `src/turn/HostedSession.cpp` (any line over 120 in the decide block), `tests/hosted/HostedDecideTests.cpp` (any line over 120). Use `awk 'length > 120 {print FILENAME":"FNR": "length}'` to find them; change only lines you can wrap without changing code.
- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset tests --clean-first && ctest --preset tests`, then `cmake --build build/dev --config RelWithDebInfo` (every dev target; `src/llama` and the tools include the turn headers).
Expected: all pass, the dev build clean.

- [ ] **Step 7: Commit** (one commit per step 1-4, one for step 5)

```bash
git add src/turn/CallTag.h src/turn/Session.cpp src/turn/HostedSession.cpp src/turn/MemoryUpkeep.cpp src/turn/Replay.cpp src/app/model/ModelSeat.cpp tests/hosted/HostedTestSupport.h
git commit -m "Call tags as constants in turn/CallTag.h, used where the tags are set and read"
```

(and similarly for the others, each staging only its files).

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`; `cmake --build build/dev --config RelWithDebInfo`; the consumer check.
- Live: `[llama]` in-process (Qwen3-4B; thinking on Qwen3.5-9B) and `[server]` over llama-server (the review case included).
- The integration summary for the Skyrim/Mantella implementer; commit and push.
