namespace MorpheusEngine.Tests.Integration.Helpers;

// Drives the teardown sequence of an integration harness so that:
//   1. Every registered step runs in the order it was registered, even if an earlier step throws.
//   2. Exceptions are collected (with the step label tagged via the exception's Data dictionary)
//      and aggregated into a single AggregateException at the end.
//
// This is the intentional replacement for the previous bare "catch { /* best-effort */ }" pattern
// that swallowed shutdown / temp-dir cleanup failures across the per-module harnesses. The audit in
// docs/LLM_TestHarnessAudit.md flagged that pattern as hiding stuck listeners and undeleted temp
// trees behind a green test result; this collector keeps the ordering guarantee while making any
// genuine teardown failure surface as a hard test failure (per CodingStyle.mdc "fail fast and loud").
//
// Usage:
//   var collector = new HarnessTeardownErrorCollector(nameof(DirectorHarness));
//   await collector.RunAsync("director.shutdown", () => _lifecycle.ShutdownAsync(...));
//   collector.Run("temp_project.dispose", () => _gameProject.Dispose());
//   collector.Run("engine_config_loader.reset", EngineConfigLoader.ResetForTesting);
//   collector.ThrowIfAny();
internal sealed class HarnessTeardownErrorCollector
{
    public const string STEP_LABEL_DATA_KEY = "MorpheusEngine.Harness.TeardownStep";

    private readonly string _harnessName;
    private readonly List<(string Label, Exception Error)> _failures = new();

    public HarnessTeardownErrorCollector(string harnessName)
    {
        if (string.IsNullOrWhiteSpace(harnessName))
        {
            throw new ArgumentException("harnessName must be non-empty.", nameof(harnessName));
        }

        _harnessName = harnessName.Trim();
    }

    // True iff at least one step has thrown since this collector was constructed.
    public bool HasFailures => _failures.Count > 0;

    // Awaits the supplied teardown step, capturing any thrown exception under stepLabel.
    // Always returns a completed task so subsequent steps can be awaited unconditionally.
    public async Task RunAsync(string stepLabel, Func<Task> step)
    {
        if (string.IsNullOrWhiteSpace(stepLabel))
        {
            throw new ArgumentException("stepLabel must be non-empty.", nameof(stepLabel));
        }

        if (step is null)
        {
            throw new ArgumentNullException(nameof(step));
        }

        try
        {
            // The step delegate itself may throw synchronously before producing a Task; both paths
            // funnel into the same catch so the collector behaves the same regardless of how the
            // failure surfaces.
            await step().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _failures.Add((stepLabel.Trim(), TagWithStepLabel(ex, stepLabel.Trim())));
        }
    }

    // Synchronous variant for steps that have no async work (e.g. IDisposable.Dispose).
    public void Run(string stepLabel, Action step)
    {
        if (string.IsNullOrWhiteSpace(stepLabel))
        {
            throw new ArgumentException("stepLabel must be non-empty.", nameof(stepLabel));
        }

        if (step is null)
        {
            throw new ArgumentNullException(nameof(step));
        }

        try
        {
            step();
        }
        catch (Exception ex)
        {
            _failures.Add((stepLabel.Trim(), TagWithStepLabel(ex, stepLabel.Trim())));
        }
    }

    // Throws an AggregateException carrying every collected failure (in registration order)
    // if any step failed; no-op otherwise.
    public void ThrowIfAny()
    {
        if (_failures.Count == 0)
        {
            return;
        }

        // The aggregate message is the load-bearing diagnostic surface in test output: it must
        // name the harness and list every failed step so a CI log makes the issue obvious without
        // unwrapping inner exceptions.
        var failedLabels = string.Join(", ", _failures.Select(failure => failure.Label));
        var message = $"{_harnessName} teardown failed: {_failures.Count} step(s) errored: {failedLabels}.";
        throw new AggregateException(message, _failures.Select(failure => failure.Error));
    }

    private static Exception TagWithStepLabel(Exception ex, string stepLabel)
    {
        // Stamp the original exception with the step label so callers that drill into the
        // AggregateException's InnerExceptions still know which teardown step produced each one.
        // Exception.Data is a public IDictionary on the base type and survives the throw / rethrow
        // in AggregateException without rewrapping.
        try
        {
            ex.Data[STEP_LABEL_DATA_KEY] = stepLabel;
        }
        catch
        {
            // Some derived exceptions override Data with an immutable dictionary; in that case the
            // label is still available through the AggregateException's per-failure ordering.
        }

        return ex;
    }
}
