# Testing Harness Remaining Roadmap

Last reviewed: 2026-05-13.

This document captures the unfinished work that previously lived in `docs/LLM_FinalTestHarnessFixSession.md`.

Phases 1 through 5 were implemented in prior sessions. The remaining major phase is Phase 6, plus lower-priority backlog items.

## Remaining high-priority phase

### Phase 6: Improve hermeticity of fixture assets

#### Objective

Stop coupling integration harnesses to the live source-tree layout for schema files and similar fixture material.

#### Work

- Replace `RepositoryRootLocator`-based schema copying with one of:
  - embedded resources in the test assembly, or
  - files copied to test output via project-file entries.
- Apply the same policy to future schema, prompt, or static fixture assets used by harnesses.
- Keep temp-project write steps, but switch the source of truth from live repo paths to declared test resources.

#### Suggested target files

- `dotnet/tests/MorpheusEngine.Tests.Integration/Director/DirectorHarness.cs`
- `dotnet/tests/MorpheusEngine.Tests.Integration/CrossCutting/EndToEndHarness.cs`
- `dotnet/tests/MorpheusEngine.Tests.Integration/MorpheusEngine.Tests.Integration.csproj`

#### Outcome

Harnesses become resilient to repo layout changes and more trustworthy as integration fixtures.

## Backlog (lower leverage than Phase 6)

### Shared mock consolidation

Consolidate near-duplicate HTTP handler mocks into one shared helper with:

- sync and async route registration
- immediate request capture
- normalized `(method, path)` matching

Candidate replacements:

- `MockHttpHandler`
- `MockOllamaHandler`
- both `MockRouterProxyHandler` copies
- reusable parts of `MockMemoryDirectorProxyHandler`

### Alternate provider lifecycle deduplication

`AlternateChatProviderHost` in `EndToEndHarness` still carries custom readiness/shutdown logic parallel to `SingleListenerLifecycle`. Long-term, align both on shared lifecycle primitives.

### Timeout consistency

`EndToEndHarness` mixes named timeout constants with hard-coded values. Standardize on one authoritative model:

- one authoritative readiness timeout, or
- separate named constants for bind readiness, post-initialize health convergence, and shutdown completion

### Outbound HttpClient disposal review

Review outbound `HttpClient` wrappers in `EndToEndHarness` to ensure harness lifetime does not quietly accumulate wrappers in long-lived test processes.

### Collector test placement

`HarnessTeardownErrorCollectorTests` currently live in the integration assembly while tagged as unit tests. Consider moving shared harness infrastructure into a common test-support assembly.

### ScriptedEmbeddingsOllama explicit vectors

Current token-to-vector heuristics are practical but implicit. If that harness grows, move to explicit scripted vectors per test.

### IntentExtractor coverage depth

Coverage for parse/normalize/validate behavior is still thinner than other major subsystems. Expand once structural harness issues are fully stabilized.

## Remaining completion condition

The original session definition of done is complete after the remaining item below is closed:

- harness fixture assets no longer depend on live repo layout
