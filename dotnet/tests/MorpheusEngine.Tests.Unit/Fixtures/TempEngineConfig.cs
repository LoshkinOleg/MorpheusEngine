namespace MorpheusEngine.Tests.Unit.Fixtures;

internal sealed class TempEngineConfig : IDisposable
{
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
        try
        {
            if (Directory.Exists(RepositoryRoot))
            {
                Directory.Delete(RepositoryRoot, recursive: true);
            }
        }
        catch
        {
            // Best-effort cleanup for temp test directories.
        }
    }
}
