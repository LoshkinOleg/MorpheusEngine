namespace MorpheusEngine.Tests.Unit.Fixtures;

internal sealed class TempEngineConfig : IDisposable
{
    private const int MAX_DELETE_ATTEMPTS = 4;
    private static readonly int[] DELETE_BACKOFF_MS = [25, 75, 150];

    public string RepositoryRoot { get; }

    public string EngineConfigPath => Path.Combine(RepositoryRoot, "engine_config.json");

    public TempEngineConfig(string engineConfigJson)
    {
        RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_cfg_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(RepositoryRoot);
        Directory.CreateDirectory(Path.Combine(RepositoryRoot, "dotnet"));

        File.WriteAllText(EngineConfigPath, engineConfigJson);
        File.WriteAllText(Path.Combine(RepositoryRoot, "dotnet", "MorpheusEngine.sln"), string.Empty);
    }

    public void Dispose()
    {
        if (!Directory.Exists(RepositoryRoot))
        {
            return;
        }

        Exception? lastException = null;
        for (var attempt = 0; attempt < MAX_DELETE_ATTEMPTS; attempt++)
        {
            try
            {
                Directory.Delete(RepositoryRoot, recursive: true);
                return;
            }
            catch (IOException ex)
            {
                // Windows file handles can outlive the test body briefly; retry before surfacing.
                lastException = ex;
            }
            catch (UnauthorizedAccessException ex)
            {
                // Antivirus/indexers can transiently lock newly created files; retry before surfacing.
                lastException = ex;
            }

            if (attempt < MAX_DELETE_ATTEMPTS - 1)
            {
                Thread.Sleep(DELETE_BACKOFF_MS[attempt]);
            }
        }

        // TempEngineConfig is often used inside helpers that intentionally assert exceptions from
        // EngineConfigLoader. Propagating dispose failures here would mask those assertions with
        // teardown noise. Keep this as an explicit best-effort exception to fail-loud policy.
        if (lastException is not null)
        {
            Console.WriteLine(
                $"[TempEngineConfig] Best-effort cleanup skipped deleting '{RepositoryRoot}' after {MAX_DELETE_ATTEMPTS} attempts: {lastException.Message}");
        }
    }
}
