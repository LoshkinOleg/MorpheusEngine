using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using FluentAssertions;
using Microsoft.Data.Sqlite;
using MorpheusEngine.Tests.Integration.Fixtures;

namespace MorpheusEngine.Tests.Integration.SessionStore;

[Trait("Category", "Integration")]
public sealed class SessionStoreHostIntegrationTests
{
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
        private readonly SessionStoreHost _host;
        private readonly HttpClient _hostHttpClient;
        private readonly Task _runTask;
        private readonly TempGameProject _gameProject;

        private SessionStoreHostHarness(TempGameProject gameProject, int port)
        {
            _gameProject = gameProject;
            Port = port;
            RunId = "test_run_001";
            RepositoryRoot = gameProject.RepositoryRoot;
            GameProjectId = gameProject.GameProjectId;
            RunDirectory = Path.Combine(RepositoryRoot, "game_projects", GameProjectId, "saved", RunId);
            DatabasePath = Path.Combine(RunDirectory, "world_state.db");

            _hostHttpClient = new HttpClient
            {
                Timeout = TimeSpan.FromSeconds(10)
            };
            _host = new SessionStoreHost(CreateConfiguration(RepositoryRoot, port), _hostHttpClient);
            Client = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{port}/"),
                Timeout = TimeSpan.FromSeconds(10)
            };
            _runTask = _host.RunAsync();
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
            var port = GetFreeTcpPort();
            var gameProject = new TempGameProject(
                gameProjectId,
                TestPayloads.MinimalManifestJson,
                loreCsv: null,
                systemInstructions: TestPayloads.MinimalSystemInstructions);
            var harness = new SessionStoreHostHarness(gameProject, port);
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
            try
            {
                if (!_runTask.IsCompleted)
                {
                    using var _ = await Client.PostAsync("/shutdown", new StringContent("{}", System.Text.Encoding.UTF8, "application/json"));
                }
            }
            catch
            {
                // Best-effort host shutdown for temp listener cleanup.
            }

            Client.Dispose();

            try
            {
                await _runTask.WaitAsync(TimeSpan.FromSeconds(5));
            }
            catch
            {
                // Best-effort wait; temp directory cleanup still needs to run.
            }

            _gameProject.Dispose();
        }

        private async Task WaitUntilReadyAsync()
        {
            var deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline)
            {
                if (_runTask.IsFaulted)
                {
                    await _runTask;
                }

                try
                {
                    using var response = await Client.GetAsync("/health");
                    if (response.StatusCode == HttpStatusCode.OK || response.StatusCode == HttpStatusCode.ServiceUnavailable)
                    {
                        return;
                    }
                }
                catch (HttpRequestException)
                {
                }
                catch (TaskCanceledException)
                {
                }

                await Task.Delay(50);
            }

            throw new TimeoutException($"SessionStoreHost did not start listening on port {Port} within the allotted time.");
        }

        private static EngineConfiguration CreateConfiguration(string repositoryRoot, int sessionStorePort)
        {
            var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
            {
                ["router"] = sessionStorePort + 10,
                ["session_store"] = sessionStorePort,
                ["memory_director"] = sessionStorePort + 1,
                ["llm_provider_qwen"] = sessionStorePort + 2,
                ["embeddings_ollama"] = sessionStorePort + 3
            };

            var modules = new[]
            {
                new EngineModuleInfo(
                    "router",
                    "Router",
                    true,
                    10,
                    new EngineModuleLaunchInfo("router.dll"),
                    []),
                new EngineModuleInfo(
                    "session_store",
                    "Session Store",
                    false,
                    50,
                    new EngineModuleLaunchInfo("session_store.dll"),
                    []),
                new EngineModuleInfo(
                    "memory_director",
                    "Memory Director",
                    true,
                    20,
                    new EngineModuleLaunchInfo("memory_director.dll"),
                    [],
                    null,
                    null,
                    new MemoryDirectorModuleOptions(12, 3210, 12, "30m")),
                new EngineModuleInfo(
                    "llm_provider_qwen",
                    "LLM Provider",
                    true,
                    30,
                    new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                    [],
                    new GenericLlmProviderModuleOptions(4096)),
                new EngineModuleInfo(
                    "embeddings_ollama",
                    "Embeddings",
                    true,
                    40,
                    new EngineModuleLaunchInfo("embeddings_ollama.dll"),
                    [],
                    null,
                    null,
                    null,
                    new EmbeddingsModuleOptions(19112, "nomic-embed-text", "30m", 2048))
            };

            var aliases = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["generic_director"] = "memory_director",
                ["generic_llm_provider"] = "llm_provider_qwen",
                ["generic_embeddings"] = "embeddings_ollama"
            };

            return new EngineConfiguration(
                repositoryRoot,
                new EnginePortMap(ports),
                modules,
                aliases,
                new Dictionary<string, EngineTurnPipelineInfo>(StringComparer.OrdinalIgnoreCase));
        }

        private static int GetFreeTcpPort()
        {
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
    }
}
