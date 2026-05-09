using System.Net;
using System.Text;

namespace MorpheusEngine.Tests.Integration.Helpers;

// Owns the (HttpClient, run task, module name, port) tuple that every per-module integration
// harness used to manage by hand. Centralizing it removes the seven copies of the same
// `WaitUntilReadyAsync` poll loop and the seven copies of the same try-catch-swallow shutdown
// dance that the harness audit (docs/LLM_TestHarnessAudit.md, lines 3-54) flagged.
//
// The lifecycle disposes the inbound `HttpClient` it is given inside ShutdownAsync's finally
// block, so callers must NOT use `using` on that client externally. The lifecycle does NOT own
// the host instance itself or any outbound clients; those remain the harness's responsibility.
//
// Intentional behavioral change vs. the previous per-harness code: every failure path here
// PROPAGATES the exception. Callers wrap ShutdownAsync inside HarnessTeardownErrorCollector to
// preserve teardown ordering while still surfacing the underlying failure.
internal sealed class SingleListenerLifecycle
{
    public HttpClient Client { get; }

    public Task RunTask { get; }

    public string ModuleName { get; }

    public int Port { get; }

    public SingleListenerLifecycle(HttpClient client, Task runTask, string moduleName, int port)
    {
        if (string.IsNullOrWhiteSpace(moduleName))
        {
            throw new ArgumentException("moduleName must be non-empty.", nameof(moduleName));
        }

        Client = client ?? throw new ArgumentNullException(nameof(client));
        RunTask = runTask ?? throw new ArgumentNullException(nameof(runTask));
        ModuleName = moduleName.Trim();
        Port = port;
    }

    // Polls /health every 50ms until either the listener responds with OK or 503 (both indicate
    // the HTTP socket is bound and the module is alive — 503 is "started but not yet initialized"),
    // or the run task faults, or the timeout expires.
    //
    // Throws:
    //   - TimeoutException with the module name and port when no health response arrives in time.
    //   - Whatever exception the run task surfaces if it has already faulted.
    public async Task WaitUntilHealthyAsync(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow.Add(timeout);
        while (DateTime.UtcNow < deadline)
        {
            // Surface a faulted run task immediately rather than waiting for the deadline so that
            // a misconfigured module fails the test with the actual host-side stack instead of a
            // generic timeout.
            if (RunTask.IsFaulted)
            {
                await RunTask.ConfigureAwait(false);
            }

            try
            {
                using var response = await Client.GetAsync("/health").ConfigureAwait(false);
                if (response.StatusCode == HttpStatusCode.OK || response.StatusCode == HttpStatusCode.ServiceUnavailable)
                {
                    return;
                }
            }
            catch (HttpRequestException)
            {
                // Listener not yet bound; retry until the deadline.
            }
            catch (TaskCanceledException)
            {
                // Per-request timeout fired before the listener was ready; retry.
            }

            await Task.Delay(50).ConfigureAwait(false);
        }

        throw new TimeoutException(
            $"{ModuleName} did not start listening on port {Port} within {timeout.TotalSeconds:0.###}s.");
    }

    // Issues POST /shutdown if the run task is still running, then waits up to waitTimeout for
    // the run task to complete. Disposes the inbound HttpClient in a finally so the test-side
    // socket is released even when shutdown throws.
    //
    // Propagates:
    //   - Any HttpRequestException / TaskCanceledException from POST /shutdown.
    //   - TimeoutException from RunTask.WaitAsync if the listener didn't terminate in time.
    //   - The run task's underlying exception if it faulted.
    public async Task ShutdownAsync(TimeSpan waitTimeout)
    {
        try
        {
            if (!RunTask.IsCompleted)
            {
                // Empty JSON body matches the protocol the harnesses used previously; modules
                // typically don't read it, but keeping the same shape minimizes behavioral drift.
                using var _ = await Client.PostAsync(
                    "/shutdown",
                    new StringContent("{}", Encoding.UTF8, "application/json")).ConfigureAwait(false);
            }

            await RunTask.WaitAsync(waitTimeout).ConfigureAwait(false);
        }
        finally
        {
            // Dispose the test-side HttpClient unconditionally so a hung shutdown doesn't leak
            // the connection pool into the next test.
            Client.Dispose();
        }
    }
}
