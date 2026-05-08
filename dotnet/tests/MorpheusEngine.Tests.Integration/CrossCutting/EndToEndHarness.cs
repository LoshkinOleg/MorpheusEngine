using Microsoft.Data.Sqlite;
using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;
using QwenProviderType = global::MorpheusEngine.LlmProviderQwen;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Integration.CrossCutting;

internal sealed class EndToEndHarness : IAsyncDisposable
{
    #region Nested types

    public sealed record OllamaChatPayload(IReadOnlyList<ChatGenerateRequest.ChatMessageDto> Messages);

    #endregion

    #region Public data

    public HttpClient RouterClient { get; }

    public HttpClient SessionStoreClient { get; }

    public HttpClient EmbeddingsClient { get; }

    public HttpClient MemoryDirectorClient { get; }

    public HttpClient? QwenClient { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public ScriptedQwenOllama? QwenOllama { get; }

    public ScriptedEmbeddingsOllama EmbeddingsOllama { get; }

    public AlternateChatProviderHost? AlternateProvider { get; }

    #endregion

    #region Private data

    private static readonly JsonSerializerOptions JSON_OPTIONS = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly TempGameProject _gameProject;
    private readonly Task _routerTask;
    private readonly Task _memoryDirectorTask;
    private readonly Task _sessionStoreTask;
    private readonly Task _embeddingsTask;
    private readonly Task? _qwenTask;
    private readonly Task? _alternateProviderTask;
    private readonly HttpClient _routerOutboundHttpClient;
    private readonly HttpClient _memoryDirectorOutboundHttpClient;
    private readonly HttpClient _sessionStoreOutboundHttpClient;
    private readonly HttpClient _embeddingsOutboundHttpClient;
    private readonly HttpClient? _qwenOutboundHttpClient;
    private readonly RouterType _routerHost;
    private readonly MemoryDirectorType _memoryDirectorHost;
    private readonly SessionStoreHost _sessionStoreHost;
    private readonly EmbeddingsOllamaModule _embeddingsHost;
    private readonly QwenProviderType? _qwenHost;

    #endregion

    #region Public methods

    public static async Task<EndToEndHarness> CreateAsync(
        bool useAlternateLlmProvider = false,
        int maxStepsPerTurn = 6,
        int maxToolResultChars = 4000,
        int maxFullMessages = 12)
    {
        var routerPort = GetFreeTcpPort();
        var memoryDirectorPort = GetFreeTcpPort();
        var sessionStorePort = GetFreeTcpPort();
        var embeddingsPort = GetFreeTcpPort();
        var qwenPort = useAlternateLlmProvider ? 0 : GetFreeTcpPort();
        var alternateProviderPort = useAlternateLlmProvider ? GetFreeTcpPort() : 0;
        var embeddingsOllamaPort = GetFreeTcpPort();
        var qwenOllamaPort = useAlternateLlmProvider ? 0 : GetFreeTcpPort();
        var gameProject = new TempGameProject(
            "test_game",
            BuildManifestJson(),
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);

        WriteMemoryDirectorFiles(gameProject);
        var configuration = CreateConfiguration(
            gameProject.RepositoryRoot,
            routerPort,
            memoryDirectorPort,
            sessionStorePort,
            embeddingsPort,
            embeddingsOllamaPort,
            qwenPort,
            qwenOllamaPort,
            alternateProviderPort,
            useAlternateLlmProvider,
            maxStepsPerTurn,
            maxToolResultChars,
            maxFullMessages);

        var harness = new EndToEndHarness(
            gameProject,
            configuration,
            routerPort,
            memoryDirectorPort,
            sessionStorePort,
            embeddingsPort,
            qwenPort,
            alternateProviderPort,
            embeddingsOllamaPort,
            qwenOllamaPort,
            useAlternateLlmProvider);
        EngineConfigLoader.ResetForTesting();
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(gameProject.RepositoryRoot);
        await harness.WaitUntilListeningAsync();
        await harness.InitializeAsync();
        return harness;
    }

    public Task<HttpResponseMessage> PostTurnAsync(int turn, string playerInput)
    {
        return RouterClient.PostAsJsonAsync("/turn", new TurnRequest(turn, playerInput));
    }

    public async Task<MemoryBlocksGetAllResponse> GetMemoryBlocksAsync(bool includeReadOnly = true)
    {
        using var response = await SessionStoreClient.PostAsJsonAsync("/memory/blocks/get_all", new MemoryBlocksGetAllRequest(includeReadOnly));
        return await ReadRequiredPayloadAsync<MemoryBlocksGetAllResponse>(response, "/memory/blocks/get_all");
    }

    public async Task<MemoryArchivalSearchResponse> SearchArchivalAsync(string query, int topK = 5, IReadOnlyList<string>? tags = null)
    {
        using var embedResponse = await EmbeddingsClient.PostAsJsonAsync("/embed", new EmbeddingRequest(string.Empty, [query]));
        var embeddings = await ReadRequiredPayloadAsync<EmbeddingResponse>(embedResponse, "/embed");
        var vector = embeddings.Vectors.Single().Vector;

        using var searchResponse = await SessionStoreClient.PostAsJsonAsync(
            "/memory/archival_search",
            new MemoryArchivalSearchRequest(query, tags, topK, vector, embeddings.Model));
        return await ReadRequiredPayloadAsync<MemoryArchivalSearchResponse>(searchResponse, "/memory/archival_search");
    }

    public SqliteConnection OpenConnection()
    {
        return RunDbInspector.OpenConnection(RepositoryRoot, GameProjectId, RunId);
    }

    public async ValueTask DisposeAsync()
    {
        await ShutdownClientAsync(RouterClient, _routerTask);
        await ShutdownClientAsync(SessionStoreClient, _sessionStoreTask);
        await ShutdownClientAsync(MemoryDirectorClient, _memoryDirectorTask);
        await ShutdownClientAsync(EmbeddingsClient, _embeddingsTask);

        if (QwenClient is not null && _qwenTask is not null)
        {
            await ShutdownClientAsync(QwenClient, _qwenTask);
        }

        if (AlternateProvider is not null && _alternateProviderTask is not null)
        {
            await AlternateProvider.DisposeAsync();
            await WaitForCompletionAsync(_alternateProviderTask);
        }

        EngineConfigLoader.ResetForTesting();
        _gameProject.Dispose();
    }

    #endregion

    #region Private methods

    private EndToEndHarness(
        TempGameProject gameProject,
        EngineConfiguration configuration,
        int routerPort,
        int memoryDirectorPort,
        int sessionStorePort,
        int embeddingsPort,
        int qwenPort,
        int alternateProviderPort,
        int embeddingsOllamaPort,
        int qwenOllamaPort,
        bool useAlternateLlmProvider)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";

        EmbeddingsOllama = new ScriptedEmbeddingsOllama();
        _embeddingsOutboundHttpClient = new HttpClient(EmbeddingsOllama.Handler, disposeHandler: false)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        if (useAlternateLlmProvider)
        {
            AlternateProvider = new AlternateChatProviderHost(alternateProviderPort);
            _alternateProviderTask = AlternateProvider.RunAsync();
        }
        else
        {
            QwenOllama = new ScriptedQwenOllama();
            _qwenOutboundHttpClient = new HttpClient(QwenOllama.Handler, disposeHandler: false)
            {
                Timeout = TimeSpan.FromSeconds(10)
            };
            _qwenHost = new QwenProviderType(configuration, _qwenOutboundHttpClient);
            _qwenHost.DisableBundledOllamaBootstrapForTesting();
            _qwenTask = _qwenHost.Run();
            QwenClient = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{qwenPort}/"),
                Timeout = TimeSpan.FromSeconds(10)
            };
        }

        _routerOutboundHttpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _memoryDirectorOutboundHttpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _sessionStoreOutboundHttpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        _routerHost = new RouterType(configuration, _routerOutboundHttpClient);
        _memoryDirectorHost = new MemoryDirectorType(configuration, _memoryDirectorOutboundHttpClient);
        _sessionStoreHost = new SessionStoreHost(configuration, _sessionStoreOutboundHttpClient);
        _embeddingsHost = new EmbeddingsOllamaModule(configuration, _embeddingsOutboundHttpClient);

        RouterClient = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{routerPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        SessionStoreClient = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{sessionStorePort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        MemoryDirectorClient = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{memoryDirectorPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        EmbeddingsClient = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{embeddingsPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };

        _routerTask = _routerHost.Run();
        _memoryDirectorTask = _memoryDirectorHost.RunAsync();
        _sessionStoreTask = _sessionStoreHost.RunAsync();
        _embeddingsTask = _embeddingsHost.RunAsync();
    }

    private async Task WaitUntilListeningAsync()
    {
        // Each module listens independently; the harness only binds the run after all listeners are accepting requests.
        await WaitUntilHealthyEndpointRespondsAsync(RouterClient, _routerTask, "router");
        await WaitUntilHealthyEndpointRespondsAsync(SessionStoreClient, _sessionStoreTask, "session_store");
        await WaitUntilHealthyEndpointRespondsAsync(EmbeddingsClient, _embeddingsTask, "embeddings_ollama");

        if (QwenClient is not null && _qwenTask is not null)
        {
            await WaitUntilHealthyEndpointRespondsAsync(QwenClient, _qwenTask, "llm_provider_qwen");
        }

        if (AlternateProvider is not null)
        {
            await WaitUntilHealthyEndpointRespondsAsync(AlternateProvider.Client, _alternateProviderTask!, "alternate_llm_provider");
        }
    }

    private async Task InitializeAsync()
    {
        var request = new InitializeModuleRequest(GameProjectId, RunId);

        using (var embeddingsResponse = await EmbeddingsClient.PostAsJsonAsync("/initialize", request))
        {
            await EnsureStatusCodeAsync(embeddingsResponse, HttpStatusCode.OK, "embeddings_ollama /initialize");
        }
        await WaitForModuleStatusAsync(EmbeddingsClient, "healthy", "embeddings_ollama");

        if (_qwenHost is not null && QwenClient is not null)
        {
            _qwenHost.SetOllamaStateForTesting(httpReady: true, ready: false, bootstrapFailed: false);
            using var qwenResponse = await QwenClient.PostAsJsonAsync("/initialize", request);
            if (qwenResponse.StatusCode is not HttpStatusCode.OK and not HttpStatusCode.Accepted)
            {
                throw new InvalidOperationException(
                    $"Unexpected llm_provider_qwen /initialize status {(int)qwenResponse.StatusCode}: {await qwenResponse.Content.ReadAsStringAsync()}");
            }

            await WaitForModuleStatusAsync(QwenClient, "healthy", "llm_provider_qwen");
        }

        if (AlternateProvider is not null)
        {
            using var altResponse = await AlternateProvider.Client.PostAsJsonAsync("/initialize", request);
            await EnsureStatusCodeAsync(altResponse, HttpStatusCode.OK, "alternate_llm_provider /initialize");
            await WaitForModuleStatusAsync(AlternateProvider.Client, "healthy", "alternate_llm_provider");
        }

        using (var sessionStoreResponse = await SessionStoreClient.PostAsJsonAsync("/initialize", request))
        {
            await EnsureStatusCodeAsync(sessionStoreResponse, HttpStatusCode.OK, "session_store /initialize");
        }
        await WaitForModuleStatusAsync(SessionStoreClient, "healthy", "session_store");

        using (var memoryDirectorResponse = await MemoryDirectorClient.PostAsJsonAsync("/initialize", request))
        {
            await EnsureStatusCodeAsync(memoryDirectorResponse, HttpStatusCode.OK, "memory_director /initialize");
        }
        await WaitForModuleStatusAsync(MemoryDirectorClient, "healthy", "memory_director");

        using (var routerResponse = await RouterClient.PostAsJsonAsync("/initialize", request))
        {
            await EnsureStatusCodeAsync(routerResponse, HttpStatusCode.OK, "router /initialize");
        }
        await WaitForModuleStatusAsync(RouterClient, "healthy", "router");
    }

    private static async Task ShutdownClientAsync(HttpClient client, Task runTask)
    {
        try
        {
            if (!runTask.IsCompleted)
            {
                using var _ = await client.PostAsync("/shutdown", new StringContent("{}", Encoding.UTF8, "application/json"));
            }
        }
        catch
        {
            // Best-effort shutdown is enough for ephemeral test listeners.
        }

        client.Dispose();
        await WaitForCompletionAsync(runTask);
    }

    private static async Task WaitForCompletionAsync(Task task)
    {
        try
        {
            await task.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch
        {
            // Listener teardown is best-effort; temp directories must still be cleaned up.
        }
    }

    private static async Task WaitUntilHealthyEndpointRespondsAsync(HttpClient client, Task runTask, string moduleName)
    {
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            if (runTask.IsFaulted)
            {
                await runTask;
            }

            try
            {
                using var response = await client.GetAsync("/health");
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

        throw new TimeoutException($"{moduleName} did not start listening within the allotted time.");
    }

    private static async Task WaitForModuleStatusAsync(HttpClient client, string expectedStatus, string moduleName)
    {
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            using var response = await client.GetAsync("/health");
            var body = await response.Content.ReadAsStringAsync();
            var payload = JsonSerializer.Deserialize<ModuleHealthResponse>(body, JSON_OPTIONS);
            if (payload is not null && string.Equals(payload.Status, expectedStatus, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"{moduleName} health did not reach status '{expectedStatus}' in time.");
    }

    private static async Task EnsureStatusCodeAsync(HttpResponseMessage response, HttpStatusCode expectedStatusCode, string operation)
    {
        if (response.StatusCode == expectedStatusCode)
        {
            return;
        }

        var body = await response.Content.ReadAsStringAsync();
        throw new InvalidOperationException(
            $"{operation} returned {(int)response.StatusCode} instead of {(int)expectedStatusCode}: {body}");
    }

    private static async Task<TResponse> ReadRequiredPayloadAsync<TResponse>(HttpResponseMessage response, string operation)
    {
        if (!response.IsSuccessStatusCode)
        {
            var body = await response.Content.ReadAsStringAsync();
            throw new InvalidOperationException($"{operation} failed with {(int)response.StatusCode}: {body}");
        }

        var payload = await response.Content.ReadFromJsonAsync<TResponse>(JSON_OPTIONS);
        return payload ?? throw new InvalidOperationException($"{operation} returned an empty JSON payload.");
    }

    private static EngineConfiguration CreateConfiguration(
        string repositoryRoot,
        int routerPort,
        int memoryDirectorPort,
        int sessionStorePort,
        int embeddingsPort,
        int embeddingsOllamaPort,
        int qwenPort,
        int qwenOllamaPort,
        int alternateProviderPort,
        bool useAlternateLlmProvider,
        int maxStepsPerTurn,
        int maxToolResultChars,
        int maxFullMessages)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["router"] = routerPort,
            ["memory_director"] = memoryDirectorPort,
            ["session_store"] = sessionStorePort,
            ["embeddings_ollama"] = embeddingsPort
        };

        if (useAlternateLlmProvider)
        {
            ports["alternate_llm_provider"] = alternateProviderPort;
        }
        else
        {
            ports["llm_provider_qwen"] = qwenPort;
        }

        var modules = new List<EngineModuleInfo>
        {
            new(
                "router",
                "Router",
                true,
                10,
                new EngineModuleLaunchInfo("router.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/turn", "POST"),
                    GetEndpoint("/proxy", "POST")
                ]),
            new(
                "memory_director",
                "Memory Director",
                true,
                20,
                new EngineModuleLaunchInfo("memory_director.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/message", "POST")
                ],
                null,
                null,
                new MemoryDirectorModuleOptions(maxStepsPerTurn, maxToolResultChars, maxFullMessages, "30m")),
            new(
                "session_store",
                "Session Store",
                true,
                30,
                new EngineModuleLaunchInfo("session_store.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/persist_turn", "POST"),
                    GetEndpoint("/memory/load_context", "POST"),
                    GetEndpoint("/memory/persist_step", "POST"),
                    GetEndpoint("/memory/recall_search", "POST"),
                    GetEndpoint("/memory/archival_search", "POST"),
                    GetEndpoint("/memory/archival_upsert", "POST"),
                    GetEndpoint("/memory/blocks/get_all", "POST"),
                    GetEndpoint("/memory/blocks/upsert", "POST"),
                    GetEndpoint("/memory/messages/recent", "POST"),
                    GetEndpoint("/memory/recall/compact", "POST")
                ]),
            new(
                "embeddings_ollama",
                "Embeddings",
                true,
                40,
                new EngineModuleLaunchInfo("embeddings_ollama.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/embed", "POST")
                ],
                null,
                null,
                null,
                new EmbeddingsModuleOptions(embeddingsOllamaPort, "nomic-embed-text", "30m", 2048))
        };

        if (useAlternateLlmProvider)
        {
            modules.Add(new EngineModuleInfo(
                "alternate_llm_provider",
                "Alternate LLM Provider",
                true,
                50,
                new EngineModuleLaunchInfo("alternate_llm_provider.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/chat", "POST"),
                    GetEndpoint("/token_count", "POST")
                ],
                new GenericLlmProviderModuleOptions(4096)));
        }
        else
        {
            modules.Add(new EngineModuleInfo(
                "llm_provider_qwen",
                "LLM Provider",
                true,
                50,
                new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                [
                    GetEndpoint("/health", "GET"),
                    GetEndpoint("/initialize", "POST"),
                    GetEndpoint("/chat", "POST"),
                    GetEndpoint("/token_count", "POST")
                ],
                new GenericLlmProviderModuleOptions(4096),
                new QwenModuleOptions(qwenOllamaPort, "qwen2.5:7b")));
        }

        var aliases = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["generic_director"] = "memory_director",
            ["generic_embeddings"] = "embeddings_ollama",
            ["generic_llm_provider"] = useAlternateLlmProvider ? "alternate_llm_provider" : "llm_provider_qwen"
        };

        var turnPipelines = new Dictionary<string, EngineTurnPipelineInfo>(StringComparer.OrdinalIgnoreCase)
        {
            ["memory_director_default"] = new(
                "memory_director_default",
                [
                    new EngineTurnPipelineStepInfo(
                        "director_message",
                        "generic_director",
                        "/message",
                        "POST",
                        "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}}}"),
                    new EngineTurnPipelineStepInfo(
                        "persist_turn",
                        "session_store",
                        "/persist_turn",
                        "POST",
                        "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}},\"directorResponseBody\":{{step.director_message.rawBodyJson}}}")
                ],
                new EngineTurnPipelineResponseMapping("director_message", "director_message_response"))
        };

        return new EngineConfiguration(
            repositoryRoot,
            new EnginePortMap(ports),
            modules,
            aliases,
            turnPipelines);
    }

    private static EngineEndpointInfo GetEndpoint(string path, string method)
    {
        return new EngineEndpointInfo(path, path, method, null, null, null);
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

    private static string BuildManifestJson()
    {
        return
            """
            {
              "id": "test_game",
              "title": "Test Game",
              "required_modules": ["generic_director", "generic_llm_provider", "generic_embeddings", "session_store"],
              "turn_pipeline": "memory_director_default"
            }
            """;
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

    #endregion

    #region Helper types

    public sealed class ScriptedQwenOllama
    {
        public MockOllamaHandler Handler { get; } = new();

        private readonly Lock _gate = new();
        private readonly Queue<Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>>> _chatHandlers = new();

        public ScriptedQwenOllama()
        {
            Handler.OnJson("GET", "/", HttpStatusCode.OK, """{"ok":true}""");
            Handler.OnAsync("POST", "/api/chat", HandleChatAsync);
            Handler.OnAsync("POST", "/api/generate", HandleGenerateAsync);
        }

        public int CapturedChatRequestCount => GetCapturedChatRequests().Count;

        public void EnqueueChatAction(string thought, string tool, string argumentsJson)
        {
            using var argumentsDocument = JsonDocument.Parse(argumentsJson);
            var actionJson = JsonSerializer.Serialize(new
            {
                thought,
                tool,
                arguments = argumentsDocument.RootElement
            });
            EnqueueChatResponse(actionJson);
        }

        public void EnqueueChatResponse(string responseText)
        {
            lock (_gate)
            {
                _chatHandlers.Enqueue((_, _) => Task.FromResult(BuildChatResponse(responseText)));
            }
        }

        public void EnqueueBlockingChatAction(
            TaskCompletionSource<object?> requestStarted,
            Task release,
            string thought,
            string tool,
            string argumentsJson)
        {
            using var argumentsDocument = JsonDocument.Parse(argumentsJson);
            var actionJson = JsonSerializer.Serialize(new
            {
                thought,
                tool,
                arguments = argumentsDocument.RootElement
            });

            lock (_gate)
            {
                _chatHandlers.Enqueue(async (_, _) =>
                {
                    requestStarted.TrySetResult(null);
                    await release;
                    return BuildChatResponse(actionJson);
                });
            }
        }

        public IReadOnlyList<OllamaChatPayload> GetCapturedChatRequests()
        {
            var payloads = new List<OllamaChatPayload>();
            foreach (var request in Handler.CapturedRequests.Where(static request => request.Path == "/api/chat"))
            {
                using var document = JsonDocument.Parse(request.Body);
                var messages = document.RootElement
                    .GetProperty("messages")
                    .EnumerateArray()
                    .Select(message => new ChatGenerateRequest.ChatMessageDto(
                        message.GetProperty("role").GetString() ?? string.Empty,
                        message.GetProperty("content").GetString() ?? string.Empty))
                    .ToArray();
                payloads.Add(new OllamaChatPayload(messages));
            }

            return payloads;
        }

        private async Task<HttpResponseMessage> HandleChatAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> handler;
            lock (_gate)
            {
                if (!_chatHandlers.TryDequeue(out handler!))
                {
                    throw new InvalidOperationException("No scripted Qwen chat response is queued.");
                }
            }

            return await handler(request, cancellationToken);
        }

        private static async Task<HttpResponseMessage> HandleGenerateAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content is null ? string.Empty : await request.Content.ReadAsStringAsync(cancellationToken);
            using var document = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
            var prompt = document.RootElement.TryGetProperty("prompt", out var promptElement)
                ? promptElement.GetString() ?? string.Empty
                : string.Empty;
            var numPredict = document.RootElement.TryGetProperty("options", out var optionsElement)
                && optionsElement.TryGetProperty("num_predict", out var numPredictElement)
                ? numPredictElement.GetInt32()
                : int.MinValue;

            if (numPredict == 0)
            {
                var tokenCount = CountApproximateTokens(prompt);
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        JsonSerializer.Serialize(new { response = string.Empty, prompt_eval_count = tokenCount, done = true }),
                        Encoding.UTF8,
                        "application/json")
                };
            }

            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("""{"response":"primed","done":true}""", Encoding.UTF8, "application/json")
            };
        }

        private static HttpResponseMessage BuildChatResponse(string responseText)
        {
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    JsonSerializer.Serialize(new
                    {
                        message = new
                        {
                            role = "assistant",
                            content = responseText
                        },
                        done = true
                    }),
                    Encoding.UTF8,
                    "application/json")
            };
        }
    }

    public sealed class ScriptedEmbeddingsOllama
    {
        public MockOllamaHandler Handler { get; } = new();

        public ScriptedEmbeddingsOllama()
        {
            Handler.OnJson("GET", "/", HttpStatusCode.OK, """{"ok":true}""");
            Handler.OnAsync("POST", "/api/embed", HandleEmbedAsync);
            Handler.OnAsync("POST", "/api/generate", HandleGenerateAsync);
        }

        private static async Task<HttpResponseMessage> HandleEmbedAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content is null ? string.Empty : await request.Content.ReadAsStringAsync(cancellationToken);
            using var document = JsonDocument.Parse(body);
            var inputs = document.RootElement.TryGetProperty("input", out var inputElement)
                ? inputElement.EnumerateArray().Select(static element => element.GetString() ?? string.Empty).ToArray()
                : Array.Empty<string>();

            var embeddings = inputs.Select(GetVectorForText).ToArray();
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    JsonSerializer.Serialize(new
                    {
                        model = "nomic-embed-text",
                        embeddings
                    }),
                    Encoding.UTF8,
                    "application/json")
            };
        }

        private static async Task<HttpResponseMessage> HandleGenerateAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content is null ? string.Empty : await request.Content.ReadAsStringAsync(cancellationToken);
            using var document = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
            var prompt = document.RootElement.TryGetProperty("prompt", out var promptElement)
                ? promptElement.GetString() ?? string.Empty
                : string.Empty;

            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    JsonSerializer.Serialize(new { response = string.Empty, prompt_eval_count = CountApproximateTokens(prompt), done = true }),
                    Encoding.UTF8,
                    "application/json")
            };
        }

        private static float[] GetVectorForText(string text)
        {
            var normalized = text.Trim().ToLowerInvariant();
            var vector = new float[4];

            if (normalized.Contains("ruin", StringComparison.Ordinal) || normalized.Contains("desert", StringComparison.Ordinal))
            {
                vector[0] += 1f;
            }

            if (normalized.Contains("oasis", StringComparison.Ordinal) || normalized.Contains("spring", StringComparison.Ordinal))
            {
                vector[1] += 1f;
            }

            if (normalized.Contains("brass", StringComparison.Ordinal)
                || normalized.Contains("key", StringComparison.Ordinal)
                || normalized.Contains("vault", StringComparison.Ordinal)
                || normalized.Contains("moon gate", StringComparison.Ordinal))
            {
                vector[2] += 1f;
            }

            if (normalized.Contains("patrol", StringComparison.Ordinal)
                || normalized.Contains("route", StringComparison.Ordinal)
                || normalized.Contains("summary", StringComparison.Ordinal))
            {
                vector[3] += 1f;
            }

            if (vector.All(static value => value == 0f))
            {
                vector[0] = 0.25f;
                vector[1] = 0.25f;
                vector[2] = 0.25f;
                vector[3] = 0.25f;
            }

            return vector;
        }
    }

    public sealed class AlternateChatProviderHost : IAsyncDisposable
    {
        private readonly HttpListener _listener = new();
        private readonly Lock _gate = new();
        private readonly Queue<ChatGenerateResponse> _chatResponses = new();
        private bool _shutdownRequested;
        private bool _runBound;

        public AlternateChatProviderHost(int port)
        {
            Client = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{port}/"),
                Timeout = TimeSpan.FromSeconds(10)
            };
            _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        }

        public HttpClient Client { get; }

        public List<ChatGenerateRequest> ChatRequests { get; } = [];

        public void EnqueueChatAction(string thought, string tool, string argumentsJson)
        {
            using var argumentsDocument = JsonDocument.Parse(argumentsJson);
            var actionJson = JsonSerializer.Serialize(new
            {
                thought,
                tool,
                arguments = argumentsDocument.RootElement
            });

            lock (_gate)
            {
                _chatResponses.Enqueue(new ChatGenerateResponse(true, actionJson, """{"provider":"alternate"}"""));
            }
        }

        public async Task RunAsync()
        {
            _listener.Start();

            try
            {
                while (!_shutdownRequested)
                {
                    var context = await _listener.GetContextAsync();
                    _ = ProcessRequestAsync(context);
                }
            }
            catch (HttpListenerException)
            {
            }
            finally
            {
                _listener.Close();
            }
        }

        public async ValueTask DisposeAsync()
        {
            try
            {
                if (!_shutdownRequested)
                {
                    using var _ = await Client.PostAsync("/shutdown", new StringContent("{}", Encoding.UTF8, "application/json"));
                }
            }
            catch
            {
            }

            Client.Dispose();
        }

        private async Task ProcessRequestAsync(HttpListenerContext context)
        {
            try
            {
                var path = context.Request.Url?.AbsolutePath ?? string.Empty;
                var method = context.Request.HttpMethod.Trim().ToUpperInvariant();

                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
                {
                    await RespondAsync(
                        context,
                        200,
                        _runBound
                            ? new ModuleHealthResponse(true, "healthy", true)
                            : new ModuleHealthResponse(false, "awaiting_initialize", false));
                    return;
                }

                if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
                {
                    _runBound = true;
                    await RespondAsync(context, 200, new InitializeModuleResponse(true));
                    return;
                }

                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase) && method == "POST")
                {
                    _shutdownRequested = true;
                    await RespondAsync(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
                    _listener.Stop();
                    return;
                }

                if (path.Equals("/chat", StringComparison.OrdinalIgnoreCase) && method == "POST")
                {
                    var request = await ReadRequestBodyAsync<ChatGenerateRequest>(context);
                    ChatRequests.Add(request);

                    ChatGenerateResponse response;
                    lock (_gate)
                    {
                        if (!_chatResponses.TryDequeue(out response!))
                        {
                            throw new InvalidOperationException("No alternate chat response is queued.");
                        }
                    }

                    await RespondAsync(context, 200, response);
                    return;
                }

                if (path.Equals("/token_count", StringComparison.OrdinalIgnoreCase) && method == "POST")
                {
                    var request = await ReadRequestBodyAsync<TokenCountRequest>(context);
                    await RespondAsync(
                        context,
                        200,
                        new TokenCountResponse(true, "alternate", CountApproximateTokens(request.Text), false));
                    return;
                }

                await RespondAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
            }
            catch (Exception e)
            {
                if (context.Response.OutputStream.CanWrite)
                {
                    await RespondAsync(context, 500, new ErrorResponse(false, "Unhandled alternate provider error.", e.Message));
                }
            }
        }

        private static async Task<T> ReadRequestBodyAsync<T>(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            var body = await reader.ReadToEndAsync();
            var payload = JsonSerializer.Deserialize<T>(body, JSON_OPTIONS);
            return payload ?? throw new InvalidOperationException("Expected a JSON payload.");
        }

        private static async Task RespondAsync(HttpListenerContext context, int statusCode, object payload)
        {
            var json = JsonSerializer.Serialize(payload);
            var bytes = Encoding.UTF8.GetBytes(json);
            context.Response.StatusCode = statusCode;
            context.Response.ContentType = "application/json";
            context.Response.ContentEncoding = Encoding.UTF8;
            context.Response.ContentLength64 = bytes.Length;
            await context.Response.OutputStream.WriteAsync(bytes);
            context.Response.Close();
        }
    }

    private static int CountApproximateTokens(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return 0;
        }

        return Math.Max(1, text.Split([' ', '\r', '\n', '\t'], StringSplitOptions.RemoveEmptyEntries).Length);
    }

    #endregion
}
