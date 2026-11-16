# Task 2 report: the pack in the host data and in HostedWorld

Status: DONE_WITH_CONCERNS (one mechanical deviation, see below)
Commit: 6404650 "Host data: the host's prompt texts, kept out of the JSON form and the hash when none are sent"

## What was implemented
- `turn::HostData::prompts` (`std::map<std::string, std::string>`), after `clock`, empty by default.
- `HostDataToJson` writes `"prompts"` only when the host sent at least one text, and only those texts. A run without a pack keeps its pre-F hash.
- `HostDataFromJson` accepts an optional `"prompts"` key (added to the unknown-key check). It must be an object of strings; faults `host data at /prompts: 'prompts' must be a JSON object of texts` and `host data at /prompts/<key>: a prompt text must be a string`. Keys and texts are left for PromptPack to check.
- `HostedWorld` gets a private `PromptPack _prompts`, filled in the constructor as step 5 via `PromptPack::FromHost(data.prompts)`, and a public `const PromptPack& Prompts() const`.
- Header comments updated for the constructor's new fault and the JSON form. `HostedWorld.h` includes `<map>` and `turn/PromptPack.h`.

## TDD evidence
- Step 1 pinned-hash check, on unchanged code (throwaway TEST_CASE, removed afterwards): `CHECK(turn::HostDataHash(test::FixtureHostData()) == "35856ce76ac0af2e")` passed: "All tests passed (1 assertion in 1 test case)". The pinned value is correct.
- RED: `cmake --build --preset tests` failed with `C2039: 'prompts': is not a member of 'morpheus::turn::HostData'` and `C2039: 'Prompts': is not a member of 'morpheus::turn::HostedWorld'` (among others).
- GREEN: `[hosted]`: "All tests passed (1411 assertions in 123 test cases)". `[prompts]`: "All tests passed (63 assertions in 6 test cases)".
- Full suite: `ctest --preset tests`: 100% tests passed, 659 of 659 (the skipped ones are the `[llama]` and `[server]` tests that need real models or servers).

## Files changed
- src/turn/HostedWorld.h
- src/turn/HostedWorld.cpp
- tests/hosted/HostedWorldTests.cpp (new TEST_CASE "Host data carries the host's prompt texts; without them it hashes as before packs"; `#include <map>`)

## Deviations from the brief
- In the new test, the two comparisons against `turn::HOSTED_RESOLVE_INSTRUCTION` and `turn::HOSTED_SPEAK_INSTRUCTION` are wrapped in `std::string(...)`. Without this the link fails: Catch2 cannot stringify `std::string_view` in this build (the note the brief gave). Same fix as Task 1.
- The brief's pinned hash `35856ce76ac0af2e` was confirmed on unchanged code; no change needed.

## Concerns
- None blocking. The `git status` warning about LF/CRLF is the existing autocrlf setup, not from this change.
- Staged nothing else; `.superpowers/sdd/progress.md`, `docs/references/*`, `settings.json`, `models/` and `imgui.ini` were left alone. This report is untracked and not staged.
