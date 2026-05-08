namespace MorpheusEngine.Tests.Integration.Fixtures;

internal sealed class TempGameProject : IDisposable
{
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
