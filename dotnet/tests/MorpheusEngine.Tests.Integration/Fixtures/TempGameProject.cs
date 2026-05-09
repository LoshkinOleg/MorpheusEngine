using Microsoft.Data.Sqlite;

namespace MorpheusEngine.Tests.Integration.Fixtures;

// Per-test disposable that materializes a self-contained game project tree (manifest + optional
// lore CSV + optional system instructions + optional engine config) under %TEMP%.
//
// Dispose policy: the previous implementation silently swallowed every exception thrown from
// Directory.Delete, which was flagged in docs/LLM_TestHarnessAudit.md (lines 3-54) as one half of
// the harness teardown blind spot. The new implementation:
//   1. Calls SqliteConnection.ClearAllPools() to release any pooled SQLite handles that the
//      Microsoft.Data.Sqlite provider retains across Dispose. This is necessary because
//      SessionStore opens the per-run world_state.db inside this directory, and the connection
//      pool keeps the file handle alive even after the host has shut down. Skipping this step
//      surfaces as IOException("being used by another process") at delete time without there
//      being any actual listener leak.
//   2. Retries the recursive delete up to MAX_DISPOSE_ATTEMPTS times with 50/100/200ms backoff
//      to absorb the genuinely transient Windows handle-release / antivirus race window.
//   3. Rethrows the last exception once retries are exhausted.
// Callers inside harnesses should wrap the Dispose call in HarnessTeardownErrorCollector so a
// genuine leak surfaces as a hard test failure without preventing the rest of the teardown
// sequence (config loader reset, listener shutdown of sibling modules, etc.) from running.
internal sealed class TempGameProject : IDisposable
{
    private const int MAX_DISPOSE_ATTEMPTS = 3;

    // Backoff in milliseconds between dispose attempts; chosen to span ~350ms total wall time,
    // which is empirically enough to clear the OS-level handle-release window without making
    // happy-path tests visibly slower.
    private static readonly int[] DISPOSE_RETRY_BACKOFF_MS = { 50, 100, 200 };

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string GameProjectDirectory => Path.Combine(RepositoryRoot, "game_projects", GameProjectId);

    public TempGameProject(
        string gameProjectId,
        string manifestJson,
        string? loreCsv = null,
        string? systemInstructions = null,
        string? engineConfigJson = null)
    {
        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_game_" + Guid.NewGuid().ToString("N"));
        GameProjectId = gameProjectId.Trim();

        Directory.CreateDirectory(Path.Combine(RepositoryRoot, "dotnet"));
        Directory.CreateDirectory(GameProjectDirectory);
        File.WriteAllText(Path.Combine(RepositoryRoot, "dotnet", "MorpheusEngine.sln"), string.Empty);
        File.WriteAllText(Path.Combine(GameProjectDirectory, "manifest.json"), manifestJson);

        if (engineConfigJson is not null)
        {
            File.WriteAllText(Path.Combine(RepositoryRoot, "engine_config.json"), engineConfigJson);
        }

        if (loreCsv is not null)
        {
            var loreDirectory = Path.Combine(GameProjectDirectory, "lore");
            Directory.CreateDirectory(loreDirectory);
            File.WriteAllText(Path.Combine(loreDirectory, "default_lore_entries.csv"), loreCsv);
        }

        if (systemInstructions is not null)
        {
            var systemDirectory = Path.Combine(GameProjectDirectory, "system");
            Directory.CreateDirectory(systemDirectory);
            File.WriteAllText(Path.Combine(systemDirectory, "instructions.md"), systemInstructions);
        }
    }

    public void Dispose()
    {
        // No-op when the directory was never created or has already been removed; keeps Dispose
        // idempotent for the rare case a harness disposes the temp project twice during fault
        // recovery.
        if (!Directory.Exists(RepositoryRoot))
        {
            return;
        }

        // Release any pooled SQLite connections that target databases under this temp tree so
        // the recursive delete below doesn't hit a "file is being used by another process" error
        // from the Microsoft.Data.Sqlite connection pool. This is a process-wide flush; it is
        // safe in tests because each TempGameProject corresponds to a single isolated run.
        SqliteConnection.ClearAllPools();

        Exception? lastException = null;
        for (var attempt = 0; attempt < MAX_DISPOSE_ATTEMPTS; attempt++)
        {
            try
            {
                Directory.Delete(RepositoryRoot, recursive: true);
                return;
            }
            catch (IOException ex)
            {
                // Typical when a child process / module hasn't released a file handle yet.
                lastException = ex;
            }
            catch (UnauthorizedAccessException ex)
            {
                // Typical on Windows when antivirus has briefly latched onto the file.
                lastException = ex;
            }

            if (attempt < MAX_DISPOSE_ATTEMPTS - 1)
            {
                Thread.Sleep(DISPOSE_RETRY_BACKOFF_MS[attempt]);
            }
        }

        // Rethrow the last observed exception so the surrounding HarnessTeardownErrorCollector
        // (or test runner, if used outside a harness) surfaces the leak instead of silently
        // accumulating temp directories under %TEMP%.
        throw lastException
            ?? new InvalidOperationException(
                $"TempGameProject failed to delete '{RepositoryRoot}' after {MAX_DISPOSE_ATTEMPTS} attempts.");
    }
}
