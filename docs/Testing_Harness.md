# Testing Harness

This repo now has a first-pass automated test harness for the .NET engine code under `dotnet/tests/`.

## Current layout

Two test projects exist:

- `dotnet/tests/MorpheusEngine.Tests.Unit`
- `dotnet/tests/MorpheusEngine.Tests.Integration`

Use them for different scopes:

- `MorpheusEngine.Tests.Unit`: pure logic, serialization, config shaping, helper behavior, and HTTP-calling classes tested with fake `HttpClient` handlers.
- `MorpheusEngine.Tests.Integration`: file-system-backed and SQLite-backed behavior that needs a temporary repo layout on disk.

The harness is intentionally split this way so fast tests stay fast and stateful tests stay explicit.

## Running tests

From the repo root:

```powershell
dotnet test dotnet/MorpheusEngine.sln
```

Run only unit tests:

```powershell
dotnet test dotnet/tests/MorpheusEngine.Tests.Unit/MorpheusEngine.Tests.Unit.csproj --filter "Category=Unit"
```

Run only integration tests:

```powershell
dotnet test dotnet/tests/MorpheusEngine.Tests.Integration/MorpheusEngine.Tests.Integration.csproj --filter "Category=Integration"
```

Repeat the last build without rebuilding:

```powershell
dotnet test dotnet/MorpheusEngine.sln --no-build
```

CI uses `.github/workflows/tests.yml` and runs the two test projects separately on `push` and `pull_request`.
That split mirrors two different execution models: highly parallel unit tests and intentionally sequential integration tests.

## What the harness includes

### Test discovery and runner config

Both test projects use:

- `xUnit`
- `Microsoft.NET.Test.Sdk`
- `xunit.runner.visualstudio`
- `FluentAssertions`

Each project has an `xunit.runner.json`, but they are intentionally different:

- `dotnet/tests/MorpheusEngine.Tests.Unit/xunit.runner.json` enables collection parallelism (`parallelizeTestCollections: true`).
- `dotnet/tests/MorpheusEngine.Tests.Integration/xunit.runner.json` disables assembly and collection parallelism (`parallelizeAssembly: false`, `parallelizeTestCollections: false`).

The integration setting is deliberate. The integration harness uses fixed listen-port assignments and process-global test seams, so deterministic sequential execution is part of the contract, not an accidental default.

### Parallelism contract

- `MorpheusEngine.Tests.Unit` is optimized for speed and broad parallel execution of mostly isolated tests.
- `MorpheusEngine.Tests.Integration` is optimized for deterministic stateful execution and therefore runs sequentially by design.
- Treat any change to integration parallelism as a behavior change: re-audit fixed ports, process-global state isolation, and harness lifecycle assumptions first.

### Shared state isolation

Some engine code is process-global, especially:

- `EngineConfigLoader`
- `EngineLog`
- `Environment.CurrentDirectory`

`EngineProcessState` is the load-bearing isolation mechanism for process-global state in this repo.

Policy by project:

- `MorpheusEngine.Tests.Unit`: tests that mutate process-global state must use `EngineProcessState`; stateless tests should not.
- `MorpheusEngine.Tests.Integration`: integration-category test classes use `EngineProcessState` by default as a broad safety rule.

This is a required isolation rule, not a style preference:

```csharp
[Trait("Category", "Unit")]
[Collection("EngineProcessState")]
public sealed class EngineConfigLoaderTests : IDisposable
{
    // ...
}
```

Collection definitions live in:

- `dotnet/tests/MorpheusEngine.Tests.Unit/Helpers/EngineProcessStateCollection.cs`
- `dotnet/tests/MorpheusEngine.Tests.Integration/Helpers/EngineProcessStateCollection.cs`

Do not use that collection for stateless unit tests, because it reduces unit-suite parallelism unnecessarily.
Sequential integration execution reduces immediate collision risk, but it does not replace consistent `EngineProcessState` annotation if runner settings change later.

### Production seams added for testing

The harness relies on a few additive testability seams in production code:

- `dotnet/src/Directory.Build.targets` grants `InternalsVisibleTo` to both test assemblies.
- `EngineConfigLoader.ResetForTesting()` clears the cached configuration.
- `EngineConfigLoader.SetRepositoryRootOverrideForTesting()` forces config/root resolution to a temp repo during tests.
- `EngineLog.ResetForTesting()` resets the console-prefixed logger state.
- Several pure helper methods were widened from `private static` to `internal static`.
- Several module constructors now have internal overloads that accept `EngineConfiguration` and/or `HttpClient`.

These exist so tests can stay deterministic without changing production runtime behavior.

## Shared test helpers

### Unit helpers

`dotnet/tests/MorpheusEngine.Tests.Unit/Helpers/MockHttpHandler.cs`

- Use this when a class makes outbound HTTP calls and you want deterministic responses.
- Register handlers by `(method, path)`.
- Wrap it in an `HttpClient` and pass that into the internal constructor of the class under test.

`dotnet/tests/MorpheusEngine.Tests.Unit/Helpers/TestConfigBuilder.cs`

- Builds `EngineConfiguration` in memory.
- Prefer this for unit tests that need ports, aliases, module rows, or turn pipelines without touching disk.

### Fixture helpers

`dotnet/tests/MorpheusEngine.Tests.Unit/Fixtures/TempEngineConfig.cs`

- Creates a disposable temporary repo root containing an `engine_config.json`.
- Best for `EngineConfigLoader` tests.

`dotnet/tests/MorpheusEngine.Tests.Integration/Fixtures/TempGameProject.cs`

- Creates a disposable `game_projects/<id>/` tree with `manifest.json`, optional lore CSV, optional system instructions, and optional temp `engine_config.json`.

`dotnet/tests/MorpheusEngine.Tests.Integration/Fixtures/RunPersistenceFixture.cs`

- Creates a disposable temp repo layout, binds `Environment.CurrentDirectory`, sets the config-loader repo-root override, initializes `RunPersistence`, and exposes a ready-to-use persistence instance.

`TestPayloads.cs`

- Central place for minimal valid manifest/config payloads used by smoke tests and future subsystem tests.

`dotnet/tests/MorpheusEngine.Tests.Integration/Helpers/MockOllamaHandler.cs`

- Use this for integration tests that run a real module listener but fake the upstream Ollama HTTP API.
- It captures outbound request bodies and supports scripted `GET`/`POST` routes, including async handlers for startup/readiness transitions.
- Prefer this over a real Ollama dependency when testing module-facing contracts such as `embeddings_ollama` or `llm_provider_qwen`.

## Existing smoke tests

The harness was validated with three smoke tests:

- `dotnet/tests/MorpheusEngine.Tests.Unit/Core/CsvRfc4180Tests.cs`
- `dotnet/tests/MorpheusEngine.Tests.Unit/Core/EngineConfigLoaderTests.cs`
- `dotnet/tests/MorpheusEngine.Tests.Integration/SessionStore/RunPersistenceIntegrationTests.cs`

These are useful examples of the expected patterns for pure unit tests, config-loader tests, and SQLite-backed integration tests.

## Test taxonomy

Use this taxonomy when deciding where tests belong:

- Unit tests: no real files, ports, listeners, or SQLite files; prefer in-memory config plus fake HTTP handlers.
- Integration tests: temp repo layout, real file I/O, SQLite-backed behavior, real module listeners, or multi-component orchestration.
- Transitional exceptions: tests that currently sit in a project whose runtime behavior does not fully match the taxonomy should be treated as temporary and tracked for reclassification.

Router layer boundary (Phase 4):

- Router unit tests cover pure helper seams (proxy payload validation, allowlist target resolution, outbound request shape, and initialize/turn precondition checks) without binding listeners.
- Router integration tests keep listener-backed HTTP contract checks (`/info`, `/health`, `/initialize`, `/turn`, `/proxy`, `/shutdown`) and lifecycle behavior.
- Reflection over private Router fields is not a preferred assertion pattern for integration tests; use externally observable API behavior instead.

## How to add a new test

Start by deciding the scope:

### Add a unit test when

- the behavior is pure or nearly pure
- the code can be exercised with in-memory config
- outbound HTTP can be mocked with `MockHttpHandler`
- no real files, ports, or SQLite files are needed

Typical locations:

- `dotnet/tests/MorpheusEngine.Tests.Unit/Core`
- `dotnet/tests/MorpheusEngine.Tests.Unit/Router`
- `dotnet/tests/MorpheusEngine.Tests.Unit/IntentExtractor`
- `dotnet/tests/MorpheusEngine.Tests.Unit/MemoryDirector`
- `dotnet/tests/MorpheusEngine.Tests.Unit/SessionStore`

### Add an integration test when

- the behavior depends on a temp repo layout
- the code reads or writes real files
- the code creates or inspects a SQLite database
- the code needs multiple components working together in a realistic environment

Typical location:

- `dotnet/tests/MorpheusEngine.Tests.Integration/<Subsystem>`

## Authoring rules

When adding new tests:

1. Put the test in the project that matches the real scope. Do not put file-system or SQLite behavior in the unit project just because it is convenient.
2. Add `[Trait("Category", "Unit")]` or `[Trait("Category", "Integration")]` at the class level.
3. Unit tests that mutate process-global state must use `[Collection("EngineProcessState")]`; stateless unit tests should not use it.
4. Integration-category tests in `MorpheusEngine.Tests.Integration` should be annotated with `[Collection("EngineProcessState")]` as the default project policy.
5. Prefer `TestConfigBuilder` over handwritten config JSON when you only need an `EngineConfiguration` object.
6. Prefer `TempEngineConfig` or `TempGameProject` when the code under test genuinely resolves files from the repo layout.
7. Prefer `MockHttpHandler` over real network calls for unit tests.
8. Keep fixture JSON minimal and valid. If you need a new commonly reused payload, add it to `TestPayloads.cs`.
9. Keep tests named with the current convention: `Type_Method_Scenario_ExpectedOutcome`.

## Teardown policy

Cleanup follows a fail-loud default so infrastructure leaks surface during the same test run that caused them.

- Default: teardown should either complete successfully or fail the test.
- For transient Windows file-handle races, use bounded retry + rethrow (do not silently swallow).
- When teardown has multiple independent steps, use `HarnessTeardownErrorCollector` so all steps run and failures aggregate.
- Keep best-effort cleanup only when the failure source is intentionally outside harness control, and document that rationale in the code at the catch site.

Current intentional exception:

- `MorpheusEngineCoreIntegrationTests.TestEnvironment.Dispose()` remains best-effort for subprocess-owned files/directories, because the spawned module processes can keep handles briefly after the parent engine exits.
- Even in this exception path, catches should be narrow (`IOException`/`UnauthorizedAccessException`) and log why cleanup was skipped.

## Policy verification quick check

Use these checks during review to detect isolation-policy drift in integration tests:

```powershell
rg '\[Trait\("Category",\s*"Integration"\)\]' dotnet/tests/MorpheusEngine.Tests.Integration --glob "*Tests.cs"
rg '\[Collection\("EngineProcessState"\)\]' dotnet/tests/MorpheusEngine.Tests.Integration --glob "*Tests.cs"
```

The integration-category class count and `EngineProcessState` class count should match. A mismatch means new integration tests were added without the collection annotation policy.

For real-port flake drift checks, use:

```powershell
rg 'AllocateFreeTcpPort|GetFreeTcpPort|new TcpListener\(IPAddress\.Loopback,\s*0\)' dotnet/tests/MorpheusEngine.Tests.Unit/Router
rg '\[Trait\("Category",\s*"Integration"\)\]' dotnet/tests/MorpheusEngine.Tests.Integration/Router --glob "*Tests.cs"
```

The first command should return no real-port helpers in unit Router tests, and the second should confirm Router socket-binding suites run under integration scope.

For Router reflection-pattern drift checks, use:

```powershell
rg 'GetPrivateField|BindingFlags\.NonPublic' dotnet/tests/MorpheusEngine.Tests.Integration/Router --glob "*Tests.cs"
```

The command should return no matches; integration Router assertions should remain HTTP-observable.

## Patterns to follow

### Testing config-loader behavior

- reset `EngineConfigLoader` before and after the test
- set `Environment.CurrentDirectory` to the temp repo
- set `EngineConfigLoader.SetRepositoryRootOverrideForTesting(tempRoot)`
- restore the original current directory in cleanup

### Testing HTTP-calling code

- create `MockHttpHandler`
- register the expected endpoint responses
- create `HttpClient` around the handler
- create the module/class with the internal constructor that accepts `HttpClient`
- assert against both the returned result and `SentRequests`

For module integration tests that expose a real `HttpListener` but depend on Ollama upstream behavior, use a dedicated harness plus `MockOllamaHandler` instead of calling a live Ollama instance. This keeps startup/health and request-shaping assertions deterministic.

### Testing session-store behavior

- use `TempGameProject` or `RunPersistenceFixture`
- assert real files or SQLite tables, not just DTOs
- keep each test isolated to its own temp directory

## Adding coverage beyond smoke tests

The next wave of tests should generally follow the original subsystem split:

- `Core`: config, contracts, CSV, logging, manifest loading
- `Router`: pipeline rendering, response mapping, proxy validation
- `IntentExtractor`: parse/normalize/validate helpers
- `MemoryDirector`: tool execution and context-budget logic
- `SessionStore`: cosine similarity, FTS query building, archival validation, view-state shaping

When in doubt, prefer direct coverage of internal helper logic before adding a larger integration test.

## Notes

- The root-resolution override in `EngineConfigLoader` is test-only and exists because the loader prefers `AppContext.BaseDirectory` before `Environment.CurrentDirectory`.
- The current CI workflow targets the two test projects directly instead of building the full solution on Linux, because `MorpheusEngine.App` is a Windows-only WPF project.
- `FluentAssertions` currently emits its Xceed license notice during test runs. That is expected with the current package choice.
