using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using MorpheusEngine.Tests.Integration.Fixtures;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;

namespace MorpheusEngine.Tests.Integration.MemoryDirector;

internal sealed class MemoryDirectorHarness : IAsyncDisposable
{
    private static readonly JsonSerializerOptions JSON_OPTIONS = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly MemoryDirectorType _host;

    private MemoryDirectorHarness(
        TempGameProject gameProject,
        int memoryDirectorPort,
        int routerPort,
        int maxStepsPerTurn,
        int maxToolResultChars,
        int maxFullMessages)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        MemoryDirectorPort = memoryDirectorPort;
        RouterPort = routerPort;

        ProxyHandler = new MockMemoryDirectorProxyHandler(maxFullMessages, maxToolResultChars);
        _outboundHttpClient = new HttpClient(ProxyHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _host = new MemoryDirectorType(
            CreateConfiguration(RepositoryRoot, memoryDirectorPort, routerPort, maxStepsPerTurn, maxToolResultChars, maxFullMessages),
            _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{memoryDirectorPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _runTask = _host.RunAsync();
    }

    public HttpClient Client { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public int MemoryDirectorPort { get; }

    public int RouterPort { get; }

    public MockMemoryDirectorProxyHandler ProxyHandler { get; }

    public static async Task<MemoryDirectorHarness> CreateAsync(
        int maxStepsPerTurn = 4,
        int maxToolResultChars = 2000,
        int maxFullMessages = 3)
    {
        var memoryDirectorPort = GetFreeTcpPort();
        var routerPort = GetFreeTcpPort();
        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);
        WriteMemoryDirectorFiles(gameProject);

        var harness = new MemoryDirectorHarness(
            gameProject,
            memoryDirectorPort,
            routerPort,
            maxStepsPerTurn,
            maxToolResultChars,
            maxFullMessages);
        await harness.WaitUntilReadyAsync();
        return harness;
    }

    public Task<HttpResponseMessage> InitializeAsync(string? gameProjectId = null, string? runId = null)
    {
        return Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest(gameProjectId ?? GameProjectId, runId ?? RunId));
    }

    public Task<HttpResponseMessage> PostMessageAsync(int turn, string playerInput)
    {
        return Client.PostAsJsonAsync("/message", new DirectorMessageRequest(turn, playerInput));
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (!_runTask.IsCompleted)
            {
                using var _ = await Client.PostAsync(
                    "/shutdown",
                    new StringContent("{}", Encoding.UTF8, "application/json"));
            }
        }
        catch
        {
            // Best-effort shutdown for temporary listeners.
        }

        Client.Dispose();

        try
        {
            await _runTask.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch
        {
            // Best-effort wait; cleanup still needs to proceed.
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

        throw new TimeoutException($"MemoryDirector did not start listening on port {MemoryDirectorPort} within the allotted time.");
    }

    private static EngineConfiguration CreateConfiguration(
        string repositoryRoot,
        int memoryDirectorPort,
        int routerPort,
        int maxStepsPerTurn,
        int maxToolResultChars,
        int maxFullMessages)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["router"] = routerPort,
            ["memory_director"] = memoryDirectorPort,
            ["session_store"] = routerPort + 1,
            ["llm_provider_qwen"] = routerPort + 2,
            ["embeddings_ollama"] = routerPort + 3
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
                "memory_director",
                "Memory Director",
                true,
                20,
                new EngineModuleLaunchInfo("memory_director.dll"),
                [],
                null,
                null,
                new MemoryDirectorModuleOptions(maxStepsPerTurn, maxToolResultChars, maxFullMessages, "30m")),
            new EngineModuleInfo(
                "session_store",
                "Session Store",
                true,
                30,
                new EngineModuleLaunchInfo("session_store.dll"),
                []),
            new EngineModuleInfo(
                "llm_provider_qwen",
                "LLM Provider",
                true,
                40,
                new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                [],
                new GenericLlmProviderModuleOptions(4096),
                new QwenModuleOptions(19112, "qwen2.5:7b")),
            new EngineModuleInfo(
                "embeddings_ollama",
                "Embeddings",
                true,
                50,
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

    private static void WriteMemoryDirectorFiles(TempGameProject gameProject)
    {
        var systemDirectory = Path.Combine(gameProject.GameProjectDirectory, "system");
        Directory.CreateDirectory(systemDirectory);
        File.WriteAllText(
            Path.Combine(systemDirectory, "agent_prompt.md"),
            """
            You are the memory-managed test game master.
            Call one tool at a time and keep narration concise.
            """);

        var schemaDirectory = Path.Combine(gameProject.RepositoryRoot, "docs", "schemas");
        Directory.CreateDirectory(schemaDirectory);
        File.WriteAllText(
            Path.Combine(schemaDirectory, "memory_director_action.schema.json"),
            File.ReadAllText(Path.Combine(GetRepositoryRoot(), "docs", "schemas", "memory_director_action.schema.json")));
    }

    private static string GetRepositoryRoot()
    {
        return Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                "..",
                "..",
                "..",
                "..",
                "..",
                ".."));
    }

    private static int GetFreeTcpPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    internal sealed class MockMemoryDirectorProxyHandler : HttpMessageHandler
    {
        private readonly int _maxFullMessages;
        private readonly int _maxToolResultChars;
        private readonly Queue<Func<ChatGenerateRequest, HttpResponseMessage>> _chatHandlers = new();

        public MockMemoryDirectorProxyHandler(int maxFullMessages, int maxToolResultChars)
        {
            _maxFullMessages = maxFullMessages;
            _maxToolResultChars = maxToolResultChars;
            LatestSnapshot = new LatestSnapshotDto(
                0,
                """{"location":"camp"}""",
                """{"narration":"The test scene is idle."}""");
        }

        public List<ModuleProxyRequest> ProxyRequests { get; } = [];

        public List<ChatGenerateRequest> ChatRequests { get; } = [];

        public List<TokenCountRequest> TokenCountRequests { get; } = [];

        public List<MemoryPersistStepRequest> PersistStepRequests { get; } = [];

        public List<MemoryCompactRecallRequest> CompactionRequests { get; } = [];

        public List<MemoryMutationDto> Mutations { get; } = [];

        public List<AgentMessageDto> Messages { get; } = [];

        public List<MemorySummaryDto> Summaries { get; } = [];

        public List<MemoryBlockDto> Blocks { get; } = [];

        public LatestSnapshotDto LatestSnapshot { get; set; }

        public void EnqueueChatAction(string thought, string tool, string argumentsJson)
        {
            using var argumentsDocument = JsonDocument.Parse(argumentsJson);
            var actionJson = JsonSerializer.Serialize(new
            {
                thought,
                tool,
                arguments = argumentsDocument.RootElement
            });
            EnqueueChatResponse(new ChatGenerateResponse(true, actionJson, """{"done":true}"""));
        }

        public void EnqueueChatResponse(ChatGenerateResponse response)
        {
            _chatHandlers.Enqueue(_ => BuildJsonHttpResponse(HttpStatusCode.OK, response));
        }

        public void EnqueueRawChatResponse(string rawBody, HttpStatusCode statusCode = HttpStatusCode.OK)
        {
            _chatHandlers.Enqueue(_ => new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(rawBody, Encoding.UTF8, "application/json")
            });
        }

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var requestBody = request.Content is null
                ? string.Empty
                : await request.Content.ReadAsStringAsync(cancellationToken);
            var proxyRequest = JsonSerializer.Deserialize<ModuleProxyRequest>(requestBody, JSON_OPTIONS)
                ?? throw new InvalidOperationException("Proxy request body must deserialize.");
            ProxyRequests.Add(proxyRequest);

            if (!string.Equals(request.Method.Method, "POST", StringComparison.OrdinalIgnoreCase)
                || !string.Equals(request.RequestUri?.AbsolutePath, "/proxy", StringComparison.Ordinal))
            {
                return BuildJsonHttpResponse(
                    HttpStatusCode.NotFound,
                    new ErrorResponse(false, "Unexpected request.", request.RequestUri?.AbsolutePath));
            }

            return proxyRequest.TargetPath switch
            {
                "/chat" => HandleChat(proxyRequest),
                "/token_count" => HandleTokenCount(proxyRequest),
                "/memory/blocks/get_all" => BuildJsonHttpResponse(HttpStatusCode.OK, new MemoryBlocksGetAllResponse(true, Blocks.ToArray())),
                "/memory/blocks/upsert" => HandleBlockUpsert(proxyRequest),
                "/memory/load_context" => HandleLoadContext(proxyRequest),
                "/memory/persist_step" => HandlePersistStep(proxyRequest),
                "/memory/messages/recent" => HandleMessagesRecent(proxyRequest),
                "/memory/recall/compact" => HandleCompaction(proxyRequest),
                _ => BuildJsonHttpResponse(
                    HttpStatusCode.NotFound,
                    new ErrorResponse(false, "No mock registered for proxy path.", proxyRequest.TargetPath))
            };
        }

        private HttpResponseMessage HandleChat(ModuleProxyRequest proxyRequest)
        {
            var chatRequest = DeserializeBody<ChatGenerateRequest>(proxyRequest);
            ChatRequests.Add(chatRequest);

            if (_chatHandlers.Count == 0)
            {
                throw new InvalidOperationException("No queued chat response was available for /chat.");
            }

            return _chatHandlers.Dequeue()(chatRequest);
        }

        private HttpResponseMessage HandleTokenCount(ModuleProxyRequest proxyRequest)
        {
            var tokenCountRequest = DeserializeBody<TokenCountRequest>(proxyRequest);
            TokenCountRequests.Add(tokenCountRequest);

            return BuildJsonHttpResponse(
                HttpStatusCode.OK,
                new TokenCountResponse(true, tokenCountRequest.Model, Math.Max(1, tokenCountRequest.Text.Length / 4), false));
        }

        private HttpResponseMessage HandleBlockUpsert(ModuleProxyRequest proxyRequest)
        {
            var upsertRequest = DeserializeBody<MemoryBlockUpsertRequest>(proxyRequest);
            var existingIndex = Blocks.FindIndex(block => string.Equals(block.Label, upsertRequest.Block.Label, StringComparison.OrdinalIgnoreCase));
            if (existingIndex >= 0)
            {
                Blocks[existingIndex] = upsertRequest.Block;
            }
            else
            {
                Blocks.Add(upsertRequest.Block);
            }

            return BuildJsonHttpResponse(HttpStatusCode.OK, new MemoryBlockUpsertResponse(true));
        }

        private HttpResponseMessage HandleLoadContext(ModuleProxyRequest proxyRequest)
        {
            var loadRequest = DeserializeBody<MemoryLoadContextRequest>(proxyRequest);
            var recentMessages = Messages
                .OrderBy(message => message.Turn)
                .ThenBy(message => message.StepNumber)
                .TakeLast(loadRequest.MaxFullMessages)
                .ToArray();

            return BuildJsonHttpResponse(
                HttpStatusCode.OK,
                new MemoryLoadContextResponse(
                    true,
                    Blocks.ToArray(),
                    recentMessages,
                    LatestSnapshot,
                    new MemoryBudgetDto(4096, 4096, _maxFullMessages, _maxToolResultChars),
                    Summaries.ToArray()));
        }

        private HttpResponseMessage HandlePersistStep(ModuleProxyRequest proxyRequest)
        {
            var persistRequest = DeserializeBody<MemoryPersistStepRequest>(proxyRequest);
            PersistStepRequests.Add(persistRequest);

            foreach (var block in persistRequest.BlockUpdates)
            {
                var existingIndex = Blocks.FindIndex(existing => string.Equals(existing.Label, block.Label, StringComparison.OrdinalIgnoreCase));
                if (existingIndex >= 0)
                {
                    Blocks[existingIndex] = block;
                }
                else
                {
                    Blocks.Add(block);
                }
            }

            Messages.AddRange(persistRequest.Messages);
            Mutations.AddRange(persistRequest.Mutations);
            return BuildJsonHttpResponse(HttpStatusCode.OK, new MemoryPersistStepResponse(true));
        }

        private HttpResponseMessage HandleMessagesRecent(ModuleProxyRequest proxyRequest)
        {
            var recentRequest = DeserializeBody<MemoryMessagesRecentRequest>(proxyRequest);
            IEnumerable<AgentMessageDto> recent = Messages
                .OrderBy(message => message.Turn)
                .ThenBy(message => message.StepNumber);

            if (recentRequest.Roles is { Count: > 0 })
            {
                recent = recent.Where(message => recentRequest.Roles.Contains(message.Role, StringComparer.OrdinalIgnoreCase));
            }

            return BuildJsonHttpResponse(
                HttpStatusCode.OK,
                new MemoryMessagesRecentResponse(true, recent.TakeLast(recentRequest.Limit).ToArray()));
        }

        private HttpResponseMessage HandleCompaction(ModuleProxyRequest proxyRequest)
        {
            var compactRequest = DeserializeBody<MemoryCompactRecallRequest>(proxyRequest);
            CompactionRequests.Add(compactRequest);

            var summary = new MemorySummaryDto(
                compactRequest.StartTurn,
                compactRequest.EndTurn,
                compactRequest.Summary,
                compactRequest.SourceMessageCount,
                compactRequest.MetadataJson);
            Summaries.Add(summary);
            Messages.RemoveAll(message => message.Turn >= compactRequest.StartTurn && message.Turn <= compactRequest.EndTurn);

            return BuildJsonHttpResponse(HttpStatusCode.OK, new MemoryCompactRecallResponse(true, summary));
        }

        private static T DeserializeBody<T>(ModuleProxyRequest proxyRequest)
        {
            if (proxyRequest.Body is null)
            {
                throw new InvalidOperationException($"Proxy request for '{proxyRequest.TargetPath}' must include a body.");
            }

            return JsonSerializer.Deserialize<T>(proxyRequest.Body.Value.GetRawText(), JSON_OPTIONS)
                ?? throw new InvalidOperationException($"Proxy body for '{proxyRequest.TargetPath}' must deserialize.");
        }

        private static HttpResponseMessage BuildJsonHttpResponse<T>(HttpStatusCode statusCode, T payload)
        {
            return new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json")
            };
        }
    }
}
