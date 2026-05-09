using System.Net;
using System.Net.Http.Json;
using FluentAssertions;
using Microsoft.Data.Sqlite;
using MorpheusEngine;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.SessionStore;

[Trait("Category", "Integration")]
public sealed class SessionStoreHostIntegrationTests
{
    // Verifies that the initialize endpoint creates the run directory and database.
    [Fact]
    public async Task SessionStoreHost_PostInitialize_CreatesRunDirectoryAndDatabase()
    {
        await using var harness = await SessionStoreHostHarness.CreateAsync();

        using var response = await harness.Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest(harness.GameProjectId, harness.RunId));

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<InitializeModuleResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();

        Directory.Exists(harness.RunDirectory).Should().BeTrue();
        File.Exists(harness.DatabasePath).Should().BeTrue();
    }

    // Verifies that the persist-turn endpoint dispatches to run persistence.
    [Fact]
    public async Task SessionStoreHost_PostPersistTurn_DispatchesToRunPersistence()
    {
        await using var harness = await SessionStoreHostHarness.CreateAsync();
        await harness.InitializeAsync();

        using var response = await harness.Client.PostAsJsonAsync(
            "/persist_turn",
            new TurnPersistRequest(1, "look around", """{"ok":true,"text":"You stand still and listen."}"""));

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<TurnPersistResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();

        using var connection = OpenConnection(harness.DatabasePath);
        CountRows(connection, "events").Should().Be(2);
        CountRows(connection, "snapshots", "turn = 1").Should().Be(1);
    }

    // Verifies that the memory load-context endpoint returns assembled memory context.
    [Fact]
    public async Task SessionStoreHost_PostMemoryLoadContext_ReturnsAssembledContext()
    {
        await using var harness = await SessionStoreHostHarness.CreateAsync();
        await harness.InitializeAsync();

        var persistence = new RunPersistence(harness.RepositoryRoot);
        _ = persistence.UpsertMemoryBlock(
            harness.GameProjectId,
            harness.RunId,
            new MemoryBlockUpsertRequest(CreateMemoryBlock("human", value: "Player prefers careful planning.", readOnly: false)));
        _ = persistence.PersistTurn(
            harness.GameProjectId,
            harness.RunId,
            new TurnPersistRequest(1, "inspect the ruins", """{"ok":true,"text":"Dust swirls through the broken archway."}"""));
        _ = persistence.AppendMessage(
            harness.GameProjectId,
            harness.RunId,
            new MemoryMessageAppendRequest(CreateAgentMessage(1, 0, "assistant", "Summary candidate message.")));
        _ = persistence.CompactRecall(
            harness.GameProjectId,
            harness.RunId,
            new MemoryCompactRecallRequest(1, 1, "The player inspected the ruins.", 1));
        _ = persistence.AppendMessage(
            harness.GameProjectId,
            harness.RunId,
            new MemoryMessageAppendRequest(CreateAgentMessage(2, 0, "assistant", "Recent message kept in context.")));

        using var response = await harness.Client.PostAsJsonAsync(
            "/memory/load_context",
            new MemoryLoadContextRequest(2, 7));

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<MemoryLoadContextResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Blocks.Should().ContainSingle(block => block.Label == "human" && block.Value == "Player prefers careful planning.");
        payload.RecentMessages.Should().ContainSingle(message => message.Content == "Recent message kept in context.");
        payload.LatestSnapshot.Turn.Should().Be(1);
        payload.Budget.NumCtx.Should().Be(4096);
        payload.Budget.TargetContextTokens.Should().Be(2867);
        payload.Budget.MaxFullMessages.Should().Be(7);
        payload.Budget.MaxToolResultChars.Should().Be(3210);
        payload.Summaries.Should().ContainSingle(summary => summary.Summary == "The player inspected the ruins.");
    }

    // Verifies that the memory persist-step endpoint saves the step data.
    [Fact]
    public async Task SessionStoreHost_PostMemoryPersistStep_PersistsStepData()
    {
        await using var harness = await SessionStoreHostHarness.CreateAsync();
        await harness.InitializeAsync();

        var request = new MemoryPersistStepRequest(
            1,
            2,
            [
                CreateAgentMessage(1, 2, "assistant", "Assistant message"),
                CreateAgentMessage(1, 2, "tool", "Tool result", messageType: "tool_result")
            ],
            [
                CreateMemoryMutation(1, 2)
            ],
            [
                CreateMemoryBlock("human", value: "Updated by memory step.", readOnly: false)
            ],
            CreateContextAccounting());

        using var response = await harness.Client.PostAsJsonAsync("/memory/persist_step", request);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<MemoryPersistStepResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();

        using var connection = OpenConnection(harness.DatabasePath);
        CountRows(connection, "agent_messages").Should().Be(2);
        CountRows(connection, "memory_mutations").Should().Be(1);
        CountRows(connection, "pipeline_events").Should().Be(1);

        var persistence = new RunPersistence(harness.RepositoryRoot);
        var blocks = persistence.GetMemoryBlocks(
            harness.GameProjectId,
            harness.RunId,
            new MemoryBlocksGetAllRequest(true));
        blocks.Blocks.Should().ContainSingle(block => block.Label == "human" && block.Value == "Updated by memory step.");
    }

    // Verifies that memory endpoints return bad requests before initialization.
    [Theory]
    [MemberData(nameof(MemoryEndpointRequests))]
    public async Task SessionStoreHost_MemoryEndpoints_BeforeInitializeReturnBadRequest(string path, object request)
    {
        await using var harness = await SessionStoreHostHarness.CreateAsync();

        using var response = await harness.Client.PostAsJsonAsync(path, request);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeFalse();
        payload.Error.Should().Contain("No bound run");
    }

    public static IEnumerable<object[]> MemoryEndpointRequests()
    {
        yield return ["/memory/load_context", new MemoryLoadContextRequest(1, 12)];
        yield return ["/memory/persist_step", new MemoryPersistStepRequest(1, 0, [], [], [])];
        yield return ["/memory/recall_search", new MemoryRecallSearchRequest("ancient ruins")];
        yield return ["/memory/archival_search", new MemoryArchivalSearchRequest("ancient ruins")];
        yield return ["/memory/archival_upsert", new MemoryArchivalUpsertRequest(CreateArchivalPassage("lore:default:test"))];
        yield return ["/memory/summaries/recent", new MemorySummariesRecentRequest(5)];
        yield return ["/memory/recall/compact", new MemoryCompactRecallRequest(1, 1, "summary", 1)];
        yield return ["/memory/blocks/get_all", new MemoryBlocksGetAllRequest(true)];
        yield return ["/memory/blocks/upsert", new MemoryBlockUpsertRequest(CreateMemoryBlock("human", readOnly: false))];
        yield return ["/memory/messages/recent", new MemoryMessagesRecentRequest(12, null)];
        yield return ["/memory/messages/append", new MemoryMessageAppendRequest(CreateAgentMessage(1, 0, "assistant", "Assistant response"))];
        yield return ["/memory/mutations/append", new MemoryMutationAppendRequest(CreateMemoryMutation(1, 0))];
        yield return ["/memory/snapshot/latest", new MemorySnapshotLatestRequest(true)];
        yield return ["/memory/pipeline_events/recent", new MemoryPipelineEventsRecentRequest(10, null)];
    }

    private static MemoryBlockDto CreateMemoryBlock(
        string label,
        string description = "Stable player-facing facts.",
        string value = "Player prefers concise descriptions.",
        int charLimit = 2000,
        bool readOnly = false)
    {
        return new MemoryBlockDto(label, description, value, charLimit, readOnly);
    }

    private static AgentMessageDto CreateAgentMessage(
        int turn,
        int stepNumber,
        string role,
        string content,
        string messageType = "send_message",
        string? toolName = null,
        string? toolCallId = null)
    {
        return new AgentMessageDto(turn, stepNumber, role, messageType, content, toolName, toolCallId);
    }

    private static MemoryMutationDto CreateMemoryMutation(
        int turn,
        int stepNumber,
        string toolName = "core_memory_append",
        string target = "human",
        string? beforeJson = null,
        string? afterJson = "{\"append\":\"Player prefers concise descriptions.\"}")
    {
        return new MemoryMutationDto(turn, stepNumber, toolName, target, beforeJson, afterJson);
    }

    private static MemoryContextAccountingDto CreateContextAccounting()
    {
        return new MemoryContextAccountingDto(
            480,
            11468,
            [],
            4096,
            2867,
            120,
            false,
            [
                new MemoryContextItemDto("agent_prompt", "system", "included", 120, 120)
            ]);
    }

    private static ArchivalPassageDto CreateArchivalPassage(
        string id,
        string scope = "project",
        string source = "lore/default_lore_entries.csv",
        string content = "Ancient ruins contain sealed northern doors.",
        IReadOnlyList<string>? tags = null,
        string? metadataJson = """{"subject":"Ancient Ruins"}""",
        string embeddingModel = "nomic-embed-text",
        IReadOnlyList<float>? embedding = null)
    {
        var vector = embedding ?? [0.12f, -0.04f, 0.88f];
        return new ArchivalPassageDto(
            id,
            scope,
            source,
            content,
            tags ?? ["lore", "seed"],
            metadataJson,
            embeddingModel,
            vector.Count,
            vector);
    }

    private static SqliteConnection OpenConnection(string dbPath)
    {
        var builder = new SqliteConnectionStringBuilder
        {
            DataSource = dbPath,
            Mode = SqliteOpenMode.ReadWriteCreate
        };

        var connection = new SqliteConnection(builder.ConnectionString);
        connection.Open();
        return connection;
    }

    private static int CountRows(SqliteConnection connection, string tableName, string? whereClause = null)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"SELECT COUNT(*) FROM {tableName}" + (string.IsNullOrWhiteSpace(whereClause) ? ";" : $" WHERE {whereClause};");
        return Convert.ToInt32(command.ExecuteScalar(), System.Globalization.CultureInfo.InvariantCulture);
    }

    private sealed class SessionStoreHostHarness : IAsyncDisposable
    {
        private static readonly TimeSpan SHUTDOWN_WAIT_TIMEOUT = TimeSpan.FromSeconds(5);
        private static readonly TimeSpan READINESS_TIMEOUT = TimeSpan.FromSeconds(5);

        private readonly SessionStoreHost _host;
        private readonly HttpClient _hostHttpClient;
        private readonly SingleListenerLifecycle _lifecycle;
        private readonly TempGameProject _gameProject;

        private SessionStoreHostHarness(TempGameProject gameProject, EngineConfiguration configuration)
        {
            _gameProject = gameProject;
            Port = configuration.GetRequiredListenPort("session_store");
            RunId = "test_run_001";
            RepositoryRoot = gameProject.RepositoryRoot;
            GameProjectId = gameProject.GameProjectId;
            RunDirectory = Path.Combine(RepositoryRoot, "game_projects", GameProjectId, "saved", RunId);
            DatabasePath = Path.Combine(RunDirectory, "world_state.db");

            _hostHttpClient = new HttpClient
            {
                Timeout = TimeSpan.FromSeconds(10)
            };
            _host = new SessionStoreHost(configuration, _hostHttpClient);
            Client = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{Port}/"),
                Timeout = TimeSpan.FromSeconds(10)
            };
            _lifecycle = new SingleListenerLifecycle(Client, _host.RunAsync(), "SessionStoreHost", Port);
        }

        public HttpClient Client { get; }

        public string RepositoryRoot { get; }

        public string GameProjectId { get; }

        public string RunId { get; }

        public string RunDirectory { get; }

        public string DatabasePath { get; }

        public int Port { get; }

        public static async Task<SessionStoreHostHarness> CreateAsync()
        {
            const string gameProjectId = "test_game";

            var document =
                IntegrationEngineConfigurationFixture.LoadConfigurationsFixture("integration_session_store_host.engine_config.json");

            var gameProject = new TempGameProject(
                gameProjectId,
                TestPayloads.MinimalManifestJson,
                loreCsv: null,
                systemInstructions: TestPayloads.MinimalSystemInstructions);

            IntegrationEngineConfigurationFixture.WriteEngineConfigJson(gameProject.RepositoryRoot, document);

            var configuration =
                IntegrationEngineConfigurationFixture.LoadConfigurationViaEngineConfigLoader(gameProject.RepositoryRoot);

            var harness = new SessionStoreHostHarness(gameProject, configuration);
            await harness.WaitUntilReadyAsync();
            return harness;
        }

        public Task InitializeAsync()
        {
            return InitializeAsync(GameProjectId, RunId);
        }

        public async Task InitializeAsync(string gameProjectId, string runId)
        {
            using var response = await Client.PostAsJsonAsync("/initialize", new InitializeModuleRequest(gameProjectId, runId));
            response.StatusCode.Should().Be(HttpStatusCode.OK);
        }

        public async ValueTask DisposeAsync()
        {
            var collector = new HarnessTeardownErrorCollector(nameof(SessionStoreHostHarness));
            await collector.RunAsync(
                "session_store.shutdown",
                () => _lifecycle.ShutdownAsync(SHUTDOWN_WAIT_TIMEOUT));
            collector.Run("temp_project.dispose", _gameProject.Dispose);
            collector.Run("engine_config_loader.reset", EngineConfigLoader.ResetForTesting);
            collector.ThrowIfAny();
        }

        private Task WaitUntilReadyAsync()
        {
            return _lifecycle.WaitUntilHealthyAsync(READINESS_TIMEOUT);
        }
    }
}
