# Build Task 4 report

Status: DONE_WITH_CONCERNS (one note, below). Commit 10ba5e4.

## Implemented
README.md, CLAUDE.md and the architecture spec updated exactly per the brief (GPU backend note, Linux section and presets, "Using Morpheus from another project", consumer-check bullets, Layout entries, `(dev and linux-release presets only)` sed, spec Build paragraph and LlamaCppBackend line). Claims checked against CMakeLists.txt, CMakePresets.json (linux-tests/linux-release cache variables, conditions), the Morpheus:: aliases in src/*/CMakeLists.txt and cmake/consumer-check/CMakeLists.txt: all match, no text changed. grep for `MORPHEUS_VULKAN`, `fails: it relies on Windows`, `build/linux/` in the three files: nothing.

## Verification (Windows)
- `tests`: no warnings, 100% passed out of 315.
- `asan`: 100% passed out of 315, no AddressSanitizer report. One linker warning LNK4300 ("ignoring /INCREMENTAL because input module contains ASAN metadata") on morpheus_tests; it comes from the asan preset's own link settings, not from this task.
- `dev` (Debug): built, no warnings in Morpheus targets (only llama.cpp's own: C4003/C4319/C5105 in _deps, ccache notice). `ctest --preset dev`: 100% passed out of 342 ([llama] cases skipped, no model variables).
- `dev` RelWithDebInfo build of morpheus, morpheus_tests, morpheus_play, morpheus_probe: exit 0, no Morpheus warnings.
- `morpheus.exe --frames 120`: exit 0 (llama.cpp load log lines only).
- Consumer check from scratch (rm -rf build/consumer-check, Visual Studio generator): configured, built, `consumer_check: ok (rolled 15, total 18)`, exit 0.

## Files changed
README.md, CLAUDE.md, docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md (committed). Nothing else staged; .superpowers and docs/references untouched.

## Deviations
None to the text. Edits made via a Python script preserving line endings (git warns LF->CRLF on README and the spec, as before).

## Linux acceptance (Step 9): NOT RUN
Nothing Linux was run; this machine cannot. The list in the brief's Step 9 is left unticked for the owner (list-presets, linux-tests 314 tests, linux-release with CUDA 13.4 and the GGML_CUDA/GGML_VULKAN cache check, optional app/ctest 341, Linux consumer check).

## Concerns
- The LNK4300 asan linker warning above (pre-existing, outside this brief's scope).
