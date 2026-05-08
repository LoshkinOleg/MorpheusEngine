using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using MorpheusEngine.Tests.Integration.Fixtures;
using DirectorType = global::MorpheusEngine.Director;

namespace MorpheusEngine.Tests.Integration.Director;

internal sealed class DirectorHarness : IAsyncDisposable
{
    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly DirectorType _host;

    private DirectorHarness(TempGameProject gameProject, int directorPort, int routerPort)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        DirectorPort = directorPort;
        RouterPort = routerPort;

        EngineConfigLoader.ResetForTesting();
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(RepositoryRoot);

        ProxyHandler = new MockRouterProxyHandler();
        _outboundHttpClient = new HttpClient(ProxyHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        _host = new DirectorType(CreateConfiguration(RepositoryRoot, directorPort, routerPort), _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{directorPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _runTask = _host.Run();
    }

    public HttpClient Client { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public int DirectorPort { get; }

    public int RouterPort { get; }

    public MockRouterProxyHandler ProxyHandler { get; }

    public static async Task<DirectorHarness> CreateAsync()
    {
        var directorPort = GetFreeTcpPort();
        var routerPort = GetFreeTcpPort();
        var engineConfigJson = BuildEngineConfigJson(directorPort, routerPort);
        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions,
            engineConfigJson);
        var harness = new DirectorHarness(gameProject, directorPort, routerPort);
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
            // Best-effort shutdown for temporary Director listeners.
        }

        Client.Dispose();

        try
        {
            await _runTask.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch
        {
            // Best-effort wait; cleanup still needs to continue.
        }

        EngineConfigLoader.ResetForTesting();
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

        throw new TimeoutException($"Director did not start listening on port {DirectorPort} within the allotted time.");
    }

    private static EngineConfiguration CreateConfiguration(string repositoryRoot, int directorPort, int routerPort)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["router"] = routerPort,
            ["director"] = directorPort,
            ["llm_provider_qwen"] = routerPort + 1,
            ["embeddings_ollama"] = routerPort + 2
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
                "director",
                "Director",
                true,
                20,
                new EngineModuleLaunchInfo("director.dll"),
                []),
            new EngineModuleInfo(
                "llm_provider_qwen",
                "LLM Provider",
                true,
                30,
                new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                [],
                new GenericLlmProviderModuleOptions(4096),
                new QwenModuleOptions(19112, "qwen2.5:7b")),
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
            ["generic_llm_provider"] = "llm_provider_qwen",
            ["generic_director"] = "director",
            ["generic_embeddings"] = "embeddings_ollama"
        };

        return new EngineConfiguration(
            repositoryRoot,
            new EnginePortMap(ports),
            modules,
            aliases,
            new Dictionary<string, EngineTurnPipelineInfo>(StringComparer.OrdinalIgnoreCase));
    }

    private static string BuildEngineConfigJson(int directorPort, int routerPort)
    {
        var config = new
        {
            module_aliases = new Dictionary<string, string>
            {
                ["generic_llm_provider"] = "llm_provider_qwen",
                ["generic_director"] = "director",
                ["generic_embeddings"] = "embeddings_ollama"
            },
            turn_pipelines = new Dictionary<string, object>
            {
                ["director_test_pipeline"] = new
                {
                    steps = new object[]
                    {
                        new
                        {
                            id = "director_message",
                            target_module = "director",
                            path = "/message",
                            method = "POST",
                            body_template = "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}}}"
                        }
                    },
                    response_mapping = new
                    {
                        source_step = "director_message",
                        type = "director_message_response"
                    }
                }
            },
            modules = new object[]
            {
                new
                {
                    port_key = "router",
                    port = routerPort,
                    load_order = 10,
                    display_name = "Router",
                    required_by_engine = true,
                    launch = "router.dll",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/proxy", description = "Proxy", method = "POST", template_contracts_id = "proxy" }
                    }
                },
                new
                {
                    port_key = "director",
                    port = directorPort,
                    load_order = 20,
                    display_name = "Director",
                    required_by_engine = true,
                    launch = "director.dll",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/message", description = "Message", method = "POST", template_contracts_id = "director_message" }
                    }
                },
                new
                {
                    port_key = "llm_provider_qwen",
                    port = routerPort + 1,
                    load_order = 30,
                    display_name = "LLM Provider",
                    required_by_engine = true,
                    launch = "llm_provider_qwen.dll",
                    num_ctx = 4096,
                    ollama_port = 19112,
                    default_chat_model = "qwen2.5:7b",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/chat", description = "Chat", method = "POST", template_contracts_id = "chat" }
                    }
                },
                new
                {
                    port_key = "embeddings_ollama",
                    port = routerPort + 2,
                    load_order = 40,
                    display_name = "Embeddings",
                    required_by_engine = true,
                    launch = "embeddings_ollama.dll",
                    ollama_port = 19112,
                    default_embedding_model = "nomic-embed-text",
                    keep_model_loaded_for = "30m",
                    embeddings_num_ctx = 2048,
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/embed", description = "Embed", method = "POST", template_contracts_id = "embeddings" }
                    }
                }
            }
        };

        return JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true });
    }

    private static int GetFreeTcpPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }
}

internal sealed class MockRouterProxyHandler : HttpMessageHandler
{
    private readonly Dictionary<(string Method, string Path), Func<HttpRequestMessage, HttpResponseMessage>> _handlers = new();

    public List<HttpRequestMessage> SentRequests { get; } = [];

    public void On(string method, string path, HttpStatusCode statusCode, string jsonBody)
    {
        On(
            method,
            path,
            _ => new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(jsonBody, Encoding.UTF8, "application/json")
            });
    }

    public void On(string method, string path, Func<HttpRequestMessage, HttpResponseMessage> handler)
    {
        if (string.IsNullOrWhiteSpace(method))
        {
            throw new ArgumentException("method must be non-empty.", nameof(method));
        }

        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("path must be non-empty.", nameof(path));
        }

        _handlers[(method.Trim().ToUpperInvariant(), NormalizePath(path))] = handler
            ?? throw new ArgumentNullException(nameof(handler));
    }

    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        SentRequests.Add(request);

        var key = (
            Method: request.Method.Method.ToUpperInvariant(),
            Path: NormalizePath(request.RequestUri?.AbsolutePath ?? "/"));

        if (_handlers.TryGetValue(key, out var handler))
        {
            return Task.FromResult(handler(request));
        }

        return Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.NotFound)
            {
                Content = new StringContent(
                    $"{{\"ok\":false,\"error\":\"No mock registered for {key.Method} {key.Path}.\"}}",
                    Encoding.UTF8,
                    "application/json")
            });
    }

    private static string NormalizePath(string path)
    {
        var trimmed = path.Trim();
        return trimmed.StartsWith("/", StringComparison.Ordinal) ? trimmed : "/" + trimmed;
    }
}
