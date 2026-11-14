# Task 10 report: documentation and full verification

Status: DONE. Commit 283b108 "Document hosted runs: HostedWorld, HostedSession, the shared memory upkeep, the hosted layers, store version 3" (docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, CLAUDE.md).

## Implemented
Applied the brief's nine spec replacements and two CLAUDE.md replacements (each target matched exactly once). The edit script preserved the file's line endings.

## Deviations (docs follow the code as built)
- Spec, `Validate` paragraph: added a sentence that `grammar::PointerToken` is public.
- Spec, store version paragraph: dropped "without migration" (the brief's text); added that each kind has its own creator (`Create`, `CreateHosted`) and its writers refuse the other kind of run with a ContractError, and that stored hosted data is shape-checked on read (StoreError naming file and column).
- Spec, Hosted runs: `Adjudicate` refuses any text the JSON parser rejects, whatever the exception, and never throws for model text; the proposal schema and grammar are built once per turn; `turn::CleanInput` is shared with `Session`; the L3 assistant line is `<speaker> [<action id> <params>]: <line>` (params only when present, as the code renders it, rather than `[<action>]`); the owned and hosted builders share `RenderAlwaysOnLore` and `RenderSummaries`.
- Spec intro of the hosted section links to `tests/hosted/fixtures/README.md` (as asked).
- Spec, Hosted tests bullet: added "its reads of damaged hosted data" to the store tests.
- Brief step 3's grep for "schema version is 2 / without migration" prints nothing, as expected.

## Byte checks (docs and CLAUDE.md)
- `LC_ALL=C grep -c $'[\x80-\xFF]'`: spec 0, CLAUDE.md 0.
- `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'`: spec 0, CLAUDE.md 0.
- The docs contain no backslash escapes.

## Verification (all foreground, one at a time)
- `cmake --build --preset tests` clean; `ctest --preset tests`: 100% passed out of 499, the 3 `[server]` tests skipped.
- `asan` preset: built (only the usual LNK4300 /INCREMENTAL notice from ASAN metadata); `ctest --preset asan`: 100% passed out of 499, same 3 skipped, no AddressSanitizer report.
- `dev`: `morpheus morpheus_tests morpheus_play morpheus_probe` built, no warnings.
- `[llama]` with the two models: 34 test cases, 28 passed, 6 skipped, 5016 assertions all passed. Skips: the thinking tests (no thinking-model variable), the `[server][llama]` test (no server URLs), and one embed-state test ("cannot hard-link the models into the temp folder", environment). The "failed to parse grammar" lines are llama.cpp stderr from grammar tests that exercise it; no failures.
- `morpheus.exe --frames 3`: exit 0.
- Consumer check: configured, built with no warnings, `consumer_check: ok (rolled 15, total 18)`.
- `tasklist | grep -i -E "morpheus|ctest"`: nothing running.
- `git status --short`: only `.superpowers/sdd/progress.md` and the docs/references submodule entries, none staged.

## Concerns
None. The test count is 499 (the brief's 488 was stale; base commit gave 499).
