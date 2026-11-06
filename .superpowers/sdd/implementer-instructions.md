# Implementer instructions (MorpheusEngine milestone 3)

You implement one task from the milestone 3 plan. MorpheusEngine is a C++20 rewrite of a local-first, LLM-driven text RPG engine.

## Requirements

Your dispatch message names your task brief. Read it first: it is your requirements, with the exact code and values to use verbatim.

Also read:
- `.superpowers/sdd/global-constraints.md`: decisions, constraints, and the model-test environment.
- `CLAUDE.md`: coding rules.

## Environment

- **Repository:** `C:/Users/user/Desktop/MorpheusEngine`, branch `cpp`, already checked out. Never switch branches or push.
- **Shell:** Windows, with the Bash tool (Git Bash) from the repo root. Visual Studio 2022 and CMake 4.4 are installed, and the Vulkan SDK 1.4.350 is installed at `C:/VulkanSDK/1.4.350.0`.
- **Presets:**
  - `tests` and `asan`: no llama.cpp. They are fast.
  - `dev`: llama.cpp with Vulkan. Its first build is long: use `run_in_background` or the max 600000 ms timeout, and split configure, build and test into separate calls.
- **Commands:**
  - Focused test: `./build/<preset>/bin/Debug/morpheus_tests.exe "[tag]"`
  - Full suite: `ctest --preset <preset>`. Run it once before committing.
- **Model-backed `[llama]` tests:** export the two variables shown in `global-constraints.md` in the same shell command as the test run.
- **Git:**
  - Never stage `dotnet/`, `models/` or `settings.json`.
  - Stage only the paths your brief's commit step names.
  - End the commit message with `-m "Co-Authored-By: Claude <your model name> 5.5 <noreply@anthropic.com>"`.

## Process

Follow the brief's steps in order: the failing test, the expected failure, the implementation, the passing tests, the commit.

The plan was written before earlier tasks ran, so files can differ slightly from what the brief assumes. Read the current file before editing it.

Small mechanical adjustments within the brief's intent are fine. Note each one in your report. Examples:
- a compile fix under /W4 /WX;
- a test helper signature.

If a fix would change a public interface, a data format, or a decision listed in `global-constraints.md`, stop. Report NEEDS_CONTEXT or BLOCKED with specifics, and do not invent a design.

## Self-review before reporting

- Is everything in the brief done?
- Is the test output clean?
- Was nothing beyond the brief added?

## Report

Writing the report is required and permitted. It goes in a git-ignored scratch folder. Write your full report to the report path your dispatch gives, including:
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
