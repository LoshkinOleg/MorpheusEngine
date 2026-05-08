using System.Text.Json;
using System.Text.Json.Nodes;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Fixtures;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
public sealed class GameProjectManifestLoaderTests
{
    [Fact]
    // Verifies that a valid manifest loads with the expected values.
    public void GameProjectManifestLoader_Load_ValidManifest_ReturnsExpectedManifest()
    {
        using var tempGameProject = new TempGameProject(TestPayloads.MinimalManifestJson);

        var manifest = GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        manifest.Id.Should().Be("test_game");
        manifest.Title.Should().Be("Test Game");
        manifest.TurnPipeline.Should().Be("memory_director_default");
        manifest.RequiredModules.Should().Equal("generic_director", "generic_llm_provider", "session_store");
    }

    [Fact]
    // Verifies that an empty game project ID is rejected.
    public void GameProjectManifestLoader_Load_EmptyGameProjectId_ThrowsArgumentException()
    {
        using var tempGameProject = new TempGameProject(TestPayloads.MinimalManifestJson);
        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, string.Empty);

        act.Should().Throw<ArgumentException>()
            .WithParameterName("gameProjectId");
    }

    [Fact]
    // Verifies that a game project ID containing '..' is rejected.
    public void GameProjectManifestLoader_Load_GameProjectIdContainingDotDot_ThrowsArgumentException()
    {
        using var tempGameProject = new TempGameProject(TestPayloads.MinimalManifestJson);
        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, "..");

        act.Should().Throw<ArgumentException>()
            .WithParameterName("gameProjectId");
    }

    [Theory]
    [InlineData("bad/id")]
    [InlineData("bad\\id")]
    // Verifies that game project IDs containing path separators are rejected.
    public void GameProjectManifestLoader_Load_GameProjectIdContainingPathSeparators_ThrowsArgumentException(string invalidGameProjectId)
    {
        using var tempGameProject = new TempGameProject(TestPayloads.MinimalManifestJson);
        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, invalidGameProjectId);

        act.Should().Throw<ArgumentException>()
            .WithParameterName("gameProjectId");
    }

    [Fact]
    // Verifies that loading a missing manifest file throws a file-not-found error.
    public void GameProjectManifestLoader_Load_MissingManifestFile_ThrowsFileNotFoundException()
    {
        using var tempGameProject = new TempGameProject(writeManifest: false);
        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<FileNotFoundException>()
            .WithMessage("*manifest.json*");
    }

    [Fact]
    // Verifies that malformed manifest JSON is wrapped in an invalid-operation error.
    public void GameProjectManifestLoader_Load_MalformedJson_ThrowsInvalidOperationExceptionWrappingJsonException()
    {
        using var tempGameProject = new TempGameProject("{ invalid json }");
        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        var exception = act.Should().Throw<InvalidOperationException>().Which;
        exception.InnerException.Should().BeOfType<JsonException>();
    }

    [Fact]
    // Verifies that the manifest ID must match the game project folder name.
    public void GameProjectManifestLoader_Load_ManifestIdNotMatchingFolderName_ThrowsInvalidOperationException()
    {
        var manifest = CreateMinimalManifestNode();
        manifest["id"] = "another_game";
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*does not match game project folder*");
    }

    [Fact]
    // Verifies that a manifest missing its ID field is rejected.
    public void GameProjectManifestLoader_Load_MissingIdField_ThrowsInvalidOperationException()
    {
        var manifest = CreateMinimalManifestNode();
        manifest.Remove("id");
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*must define a non-empty 'id'*");
    }

    [Fact]
    // Verifies that a manifest missing its title field is rejected.
    public void GameProjectManifestLoader_Load_MissingTitleField_ThrowsInvalidOperationException()
    {
        var manifest = CreateMinimalManifestNode();
        manifest.Remove("title");
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*must define a non-empty 'title'*");
    }

    [Fact]
    // Verifies that required_modules cannot contain empty entries.
    public void GameProjectManifestLoader_Load_RequiredModulesContainingEmptyEntry_ThrowsInvalidOperationException()
    {
        var manifest = CreateMinimalManifestNode();
        manifest["required_modules"] = new JsonArray("generic_director", "", "session_store");
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*empty entry under required_modules*");
    }

    [Fact]
    // Verifies that required_modules cannot contain '..' entries.
    public void GameProjectManifestLoader_Load_RequiredModulesContainingDotDot_ThrowsInvalidOperationException()
    {
        var manifest = CreateMinimalManifestNode();
        manifest["required_modules"] = new JsonArray("generic_director", "..", "session_store");
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var act = () => GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*contains invalid module key '..'*");
    }

    [Fact]
    // Verifies that the default turn pipeline is used when the manifest omits one.
    public void GameProjectManifestLoader_Load_MissingTurnPipeline_UsesDefault()
    {
        var manifest = CreateMinimalManifestNode();
        manifest.Remove("turn_pipeline");
        using var tempGameProject = new TempGameProject(ToJson(manifest));

        var loadedManifest = GameProjectManifestLoader.Load(tempGameProject.RepositoryRoot, tempGameProject.GameProjectId);

        loadedManifest.TurnPipeline.Should().Be("memory_director_default");
    }

    private static JsonObject CreateMinimalManifestNode()
    {
        return JsonNode.Parse(TestPayloads.MinimalManifestJson)!.AsObject();
    }

    private static string ToJson(JsonObject manifest)
    {
        return manifest.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true
        });
    }

    private sealed class TempGameProject : IDisposable
    {
        public string RepositoryRoot { get; }

        public string GameProjectId { get; }

        public TempGameProject(string manifestJson, string gameProjectId = "test_game")
            : this(gameProjectId, manifestJson, writeManifest: true)
        {
        }

        public TempGameProject(bool writeManifest, string gameProjectId = "test_game")
            : this(gameProjectId, TestPayloads.MinimalManifestJson, writeManifest)
        {
        }

        private TempGameProject(string gameProjectId, string manifestJson, bool writeManifest)
        {
            RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_manifest_" + Guid.NewGuid().ToString("N"));
            GameProjectId = gameProjectId;

            var gameProjectDirectory = Path.Combine(RepositoryRoot, "game_projects", GameProjectId);
            Directory.CreateDirectory(Path.Combine(RepositoryRoot, "dotnet"));
            Directory.CreateDirectory(gameProjectDirectory);
            File.WriteAllText(Path.Combine(RepositoryRoot, "dotnet", "MorpheusEngine.sln"), string.Empty);

            if (writeManifest)
            {
                File.WriteAllText(Path.Combine(gameProjectDirectory, "manifest.json"), manifestJson);
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
                // Best-effort cleanup for temp game project directories.
            }
        }
    }
}
