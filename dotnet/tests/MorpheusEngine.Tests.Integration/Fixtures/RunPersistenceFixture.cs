using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.Fixtures;

internal sealed class RunPersistenceFixture : IDisposable
{
    private readonly string _originalCurrentDirectory;
    private readonly TempGameProject _gameProject;

    public string RepositoryRoot => _gameProject.RepositoryRoot;

    public string GameProjectId { get; }

    public string RunId { get; }

    public RunPersistence Persistence { get; }

    public RunPersistenceFixture(
        string gameProjectId = "test_game",
        string runId = "test_run_001",
        string? manifestJson = null,
        string? loreCsv = null,
        string? engineConfigJson = null)
    {
        GameProjectId = gameProjectId;
        RunId = runId;
        _originalCurrentDirectory = Environment.CurrentDirectory;

        _gameProject = new TempGameProject(
            gameProjectId,
            manifestJson ?? TestPayloads.MinimalManifestJson,
            loreCsv ?? TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions,
            engineConfigJson ?? TestPayloads.BuildMinimalEngineConfigJson());

        Environment.CurrentDirectory = RepositoryRoot;
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(RepositoryRoot);
        Persistence = new RunPersistence(RepositoryRoot);
        Persistence.InitializeRun(GameProjectId, RunId);
    }

    public void Dispose()
    {
        var teardown = new HarnessTeardownErrorCollector(nameof(RunPersistenceFixture));
        teardown.Run("reset EngineConfigLoader", EngineConfigLoader.ResetForTesting);
        teardown.Run("restore current directory", () => Environment.CurrentDirectory = _originalCurrentDirectory);
        teardown.Run("dispose temp game project", _gameProject.Dispose);
        teardown.ThrowIfAny();
    }
}
