using Microsoft.Data.Sqlite;
using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using MorpheusEngine;
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

    private static readonly TimeSpan SHUTDOWN_WAIT_TIMEOUT = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan READINESS_TIMEOUT = TimeSpan.FromSeconds(5);

    private readonly TempGameProject _gameProject;
    // Per-listener lifecycles in startup order. Stored as a list so DisposeAsync can route every
    // shutdown through HarnessTeardownErrorCollector with a single foreach instead of one
    // hand-rolled call per module.
    private readonly List<SingleListenerLifecycle> _lifecycles = new();
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
        var gameProject = new TempGameProject(
            "test_game",
            BuildManifestJson(),
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);

        WriteMemoryDirectorFiles(gameProject);

        var fixtureName = useAlternateLlmProvider
            ? "integration_end_to_end_alternate.engine_config.json"
            : "integration_end_to_end_qwen.engine_config.json";

        var configDocument = IntegrationEngineConfigurationFixture.LoadConfigurationsFixture(fixtureName);
        IntegrationEngineConfigurationFixture.PatchMemoryDirectorOptions(
            configDocument,
            maxStepsPerTurn,
            maxToolResultChars,
            maxFullMessages);

        IntegrationEngineConfigurationFixture.WriteEngineConfigJson(gameProject.RepositoryRoot, configDocument);
        var configuration =
            IntegrationEngineConfigurationFixture.LoadConfigurationViaEngineConfigLoader(gameProject.RepositoryRoot);

        var harness = new EndToEndHarness(gameProject, configuration, useAlternateLlmProvider);
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
        // Same teardown contract as the per-module harnesses: every step runs in registration
        // order, errors are collected, and the AggregateException at the end fails the test if
        // anything went wrong. The lifecycles already shut down listeners in router-first order
        // so cross-module dependencies (router -> memory_director -> session_store) unwind in
        // a sane sequence.
        var collector = new HarnessTeardownErrorCollector(nameof(EndToEndHarness));
        foreach (var lifecycle in _lifecycles)
        {
            await collector.RunAsync(
                $"{lifecycle.ModuleName}.shutdown",
                () => lifecycle.ShutdownAsync(SHUTDOWN_WAIT_TIMEOUT));
        }

        if (AlternateProvider is not null && _alternateProviderTask is not null)
        {
            // AlternateChatProviderHost owns its own HttpListener-based shutdown path and has a
            // dedicated DisposeAsync; route both through the collector so its failures surface
            // identically to the lifecycle-managed listeners.
            await collector.RunAsync(
                "alternate_llm_provider.dispose",
                () => AlternateProvider.DisposeAsync().AsTask());
            await collector.RunAsync(
                "alternate_llm_provider.wait_for_completion",
                () => _alternateProviderTask.WaitAsync(SHUTDOWN_WAIT_TIMEOUT));
        }

        collector.Run("temp_project.dispose", _gameProject.Dispose);
        collector.Run("engine_config_loader.reset", EngineConfigLoader.ResetForTesting);
        collector.ThrowIfAny();
    }

    #endregion

    #region Private methods

    private EndToEndHarness(TempGameProject gameProject, EngineConfiguration configuration, bool useAlternateLlmProvider)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";

        var routerPort = configuration.GetRequiredListenPort("router");
        var memoryDirectorPort = configuration.GetRequiredListenPort("memory_director");
        var sessionStorePort = configuration.GetRequiredListenPort("session_store");
        var embeddingsPort = configuration.GetRequiredListenPort("embeddings_ollama");
        var qwenPort = useAlternateLlmProvider ? 0 : configuration.GetRequiredListenPort("llm_provider_qwen");
        var alternateProviderPort = useAlternateLlmProvider
            ? configuration.GetRequiredListenPort("alternate_llm_provider")
            : 0;

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
            // Qwen host construction happens here, but Run() is called later when the lifecycle
            // entry is appended so it stays consistent with the other listeners and isn't started
            // twice.
            QwenOllama = new ScriptedQwenOllama();
            _qwenOutboundHttpClient = new HttpClient(QwenOllama.Handler, disposeHandler: false)
            {
                Timeout = TimeSpan.FromSeconds(10)
            };
            _qwenHost = new QwenProviderType(configuration, _qwenOutboundHttpClient);
            _qwenHost.DisableBundledOllamaBootstrapForTesting();
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

        // Lifecycle order matters during teardown: router unbinds first so it stops dispatching
        // turns into the memory_director / session_store / embeddings modules before they are
        // told to shut down. Qwen, when present, is the leaf provider so it goes last.
        _lifecycles.Add(new SingleListenerLifecycle(RouterClient, _routerHost.Run(), "router", routerPort));
        _lifecycles.Add(new SingleListenerLifecycle(MemoryDirectorClient, _memoryDirectorHost.RunAsync(), "memory_director", memoryDirectorPort));
        _lifecycles.Add(new SingleListenerLifecycle(SessionStoreClient, _sessionStoreHost.RunAsync(), "session_store", sessionStorePort));
        _lifecycles.Add(new SingleListenerLifecycle(EmbeddingsClient, _embeddingsHost.RunAsync(), "embeddings_ollama", embeddingsPort));
        if (QwenClient is not null && _qwenHost is not null)
        {
            _lifecycles.Add(new SingleListenerLifecycle(QwenClient, _qwenHost.Run(), "llm_provider_qwen", qwenPort));
        }
    }

    private async Task WaitUntilListeningAsync()
    {
        // Each module listens independently; the harness only binds the run after all listeners
        // are accepting requests. Iterating the lifecycle list keeps this in lockstep with the
        // listener set declared in the constructor.
        foreach (var lifecycle in _lifecycles)
        {
            await lifecycle.WaitUntilHealthyAsync(READINESS_TIMEOUT);
        }

        if (AlternateProvider is not null && _alternateProviderTask is not null)
        {
            // AlternateChatProviderHost predates SingleListenerLifecycle (HttpListener-based, not
            // ModuleHost-based) and is wired up by hand. The polling loop below mirrors what the
            // lifecycle helper does for the in-process module hosts.
            await WaitUntilAlternateProviderHealthyAsync(_alternateProviderTask);
        }
    }

    private async Task WaitUntilAlternateProviderHealthyAsync(Task runTask)
    {
        var deadline = DateTime.UtcNow.Add(READINESS_TIMEOUT);
        while (DateTime.UtcNow < deadline)
        {
            if (runTask.IsFaulted)
            {
                await runTask.ConfigureAwait(false);
            }

            try
            {
                using var response = await AlternateProvider!.Client.GetAsync("/health");
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

        throw new TimeoutException(
            $"alternate_llm_provider did not start listening within {READINESS_TIMEOUT.TotalSeconds:0.###}s.");
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

        var schemaDirectory = Path.Combine(
            gameProject.RepositoryRoot,
            "dotnet",
            "src",
            "MorpheusEngine.LlmProvider_qwen",
            "schemas");
        Directory.CreateDirectory(schemaDirectory);
        File.WriteAllText(
            Path.Combine(schemaDirectory, "memory_director_action.schema.json"),
            File.ReadAllText(
                Path.Combine(
                    RepositoryRootLocator.GetRepositoryRoot(),
                    "dotnet",
                    "src",
                    "MorpheusEngine.LlmProvider_qwen",
                    "schemas",
                    "memory_director_action.schema.json")));
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
            // Mirror the SingleListenerLifecycle contract: propagate POST /shutdown failures so
            // the surrounding HarnessTeardownErrorCollector can surface them, and dispose the
            // Client unconditionally via finally so a hung shutdown doesn't leak the socket.
            try
            {
                if (!_shutdownRequested)
                {
                    using var _ = await Client.PostAsync("/shutdown", new StringContent("{}", Encoding.UTF8, "application/json"));
                }
            }
            finally
            {
                Client.Dispose();
            }
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
