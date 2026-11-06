# Implementer instructions (MorpheusEngine milestone 5)

You implement one task from the milestone 5 plan (narration review, compact transcript, scene retrieval). MorpheusEngine is a local-first, LLM-driven text RPG engine in C++20.

## Requirements

Your dispatch message names your task brief. Read it first: it is your requirements, with the exact code and values to use verbatim.

Also read:
- `.superpowers/sdd/m5-global-constraints.md`: the plan's global constraints and the decisions it made.
- `CLAUDE.md`: coding rules.

## Environment

- **Repository:** `D:/MorpheusEngine`, branch `master`, already checked out. Never switch branches, never push.
- **Shell:** Windows, with the Bash tool (Git Bash) from the repo root. Visual Studio 2022 and CMake are installed.
- **Presets:** `tests` and `asan` (fast, no llama.cpp). `dev` (llama.cpp with Vulkan, the app, `morpheus_play`) is already configured in `build/dev`; build a `dev` target only when your brief says so, with `cmake --build build/dev --config RelWithDebInfo --target <target>` and the max 600000 ms timeout.
- **Commands:**
  - Focused test: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`
  - Full suite: `ctest --preset tests`. Run it once before committing.
- **Git:**
  - Never stage `settings.json`, `models/`, `imgui.ini`, `.superpowers/` or `docs/references/` (the reference submodules show as modified; leave them alone).
  - Stage only the paths your brief's commit step names.
  - End the commit message with `-m "Co-Authored-By: Claude <your model name> 5.5 <noreply@anthropic.com>"`.

## Process

Follow the brief's steps in order: the failing test, the expected failure, the implementation, the passing tests, the commit.

The plan was written before earlier tasks ran, so files can differ slightly from what the brief assumes. Read the current file before editing it.

Small mechanical adjustments within the brief's intent are fine. Note each one in your report. Examples:
- a compile fix under /W4 /WX;
- a test helper signature.

If a fix would change a public interface, a data format, or a decision listed in `m5-global-constraints.md`, stop. Report NEEDS_CONTEXT or BLOCKED with specifics, and do not invent a design.

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
