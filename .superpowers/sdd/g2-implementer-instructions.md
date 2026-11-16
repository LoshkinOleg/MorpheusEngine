# Implementer instructions (MorpheusEngine generation controls)

You implement one task from the generation-controls plan (sub-project G2 of the Skyrim/Mantella integration: a min_p sampling field, an explicit call tag on every turn-layer model call, and the thinking block's markers as settings data). MorpheusEngine is a local-first, LLM-driven text RPG engine in C++20.

## Requirements

Your dispatch message names your task brief. Read it first: it is your requirements, with the exact code and values to use verbatim.

Also read:
- `.superpowers/sdd/g2-global-constraints.md`: the plan's global constraints and the decisions it made.
- `CLAUDE.md`: coding rules.

## Environment

- **Repository:** `D:/MorpheusEngine`, branch `master`, already checked out. Never switch branches, never push.
- **Shell:** Windows, with the Bash tool (Git Bash) from the repo root. Visual Studio 2022 and CMake are installed.
- **Presets:** `tests` and `asan` (fast, no llama.cpp). `dev` (llama.cpp with Vulkan, the app, `morpheus_play`) is already configured in `build/dev`; build a `dev` target only when your brief says so, with `cmake --build build/dev --config RelWithDebInfo --target <target>` and the max 600000 ms timeout.
- **Commands:**
  - Focused test: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`
  - Full suite: `ctest --preset tests`. Run it once before committing.
- **Git:**
  - Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/` (the reference submodules show as modified; leave them alone, and commit with `git commit <paths>` so only your files go in).
  - Stage only the paths your brief's commit step names.
  - End the commit message with `-m "Co-Authored-By: Claude <your model name> 5.5 <noreply@anthropic.com>"`.

## Process

Follow the brief's steps in order: the failing test, the expected failure, the implementation, the passing tests, the commit.

The plan was written before earlier tasks ran, so files can differ slightly from what the brief assumes. Read the current file before editing it.

Small mechanical adjustments within the brief's intent are fine. Note each one in your report. Examples:
- a compile fix under /W4 /WX;
- a test helper signature.

If a fix would change a public interface, a data format, or a decision listed in `g2-global-constraints.md`, stop. Report NEEDS_CONTEXT or BLOCKED with specifics, and do not invent a design.

## Self-review before reporting

- Is everything in the brief done?
- Is the test output clean (no warnings)?
- Was nothing beyond the brief added?

## Report

Write your full report to the report path your dispatch gives (under `.superpowers/sdd/`; do not stage it), including:
- what you implemented;
- TDD evidence: the RED command and its output, then the GREEN command and its output;
- files changed;
- deviations from the brief, and why;
- concerns.

Then reply with ONLY these items, under 15 lines in total:
- Status: DONE | DONE_WITH_CONCERNS | BLOCKED | NEEDS_CONTEXT
- commits (short SHA and subject)
- a one-line test summary
- concerns
- the report path

## Escape sequences: check the bytes you wrote

Your file-writing and editing tools may silently decode backslash-u (four hex digits) and backslash-x (two hex digits) sequences in your input into the real characters. A decoded escape in a string or a test a decoded one silently changes what the code means.

- After every write or edit of a source or test file, run `LC_ALL=C grep -c $'[\x80-\xFF]' <file>`: it must print 0 (CLAUDE.md requires ASCII-only sources). A decoded escape can also become a control byte, which that check misses, so also run `LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' <file>`: it must print 0.
- Then grep for the escapes you meant to write and check they are still the literal backslash sequences.
- If a tool keeps decoding them, build the text from a `BACKSLASH` constant (`const std::string BACKSLASH = "\\";` then `BACKSLASH + "u00e9"`), or write the file through a shell heredoc with a quoted delimiter (`<<'EOF'`).
- Report both greps' output.

## Builds and background work

- Run builds and tests in the foreground with the 600000 ms timeout, one at a time. Never start a second test run while one is going.
- Leave nothing running in the background when you report (a running test binary locks it for the next task's build).

## Builds

Run every build and test in the foreground with the 600000 ms timeout. Never pass run_in_background for a build or a test run: a background build once held the compiler's PDB and blocked the next build for minutes.

After a change to a struct used across files, build once with `cmake --build --preset tests --clean-first`. If a test you did not touch crashes (SIGSEGV or a "Debug Assertion Failed" dialog), clean-rebuild before changing code.
