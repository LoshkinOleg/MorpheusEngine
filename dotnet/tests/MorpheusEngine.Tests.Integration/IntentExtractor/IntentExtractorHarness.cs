using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using MorpheusEngine.Tests.Integration.Fixtures;
using IntentExtractorType = global::MorpheusEngine.IntentExtractor;

namespace MorpheusEngine.Tests.Integration.IntentExtractor;

internal sealed class IntentExtractorHarness : IAsyncDisposable
{
    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly IntentExtractorType _host;

    private IntentExtractorHarness(TempGameProject gameProject, int intentExtractorPort, int routerPort)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        IntentExtractorPort = intentExtractorPort;
        RouterPort = routerPort;

        ProxyHandler = new MockRouterProxyHandler();
        _outboundHttpClient = new HttpClient(ProxyHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _host = new IntentExtractorType(CreateConfiguration(RepositoryRoot, intentExtractorPort, routerPort), _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{intentExtractorPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _runTask = _host.Run();
    }

    public HttpClient Client { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public int IntentExtractorPort { get; }

    public int RouterPort { get; }

    public MockRouterProxyHandler ProxyHandler { get; }

    public static async Task<IntentExtractorHarness> CreateAsync()
    {
        var intentExtractorPort = GetFreeTcpPort();
        var routerPort = GetFreeTcpPort();
        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            loreCsv: null,
            systemInstructions: null);
        var harness = new IntentExtractorHarness(gameProject, intentExtractorPort, routerPort);
        await harness.WaitUntilReadyAsync();
        return harness;
    }

    public Task<HttpResponseMessage> InitializeAsync(string? gameProjectId = null, string? runId = null)
    {
        return Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest(gameProjectId ?? GameProjectId, runId ?? RunId));
    }

    public Task<HttpResponseMessage> PostIntentAsync(string playerInput)
    {
        return Client.PostAsJsonAsync("/intent", new IntentRequest(playerInput));
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
            // Best-effort listener shutdown for temporary integration hosts.
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

        throw new TimeoutException($"IntentExtractor did not start listening on port {IntentExtractorPort} within the allotted time.");
    }

    private static EngineConfiguration CreateConfiguration(string repositoryRoot, int intentExtractorPort, int routerPort)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["router"] = routerPort,
            ["intent_extractor"] = intentExtractorPort,
            ["llm_provider_qwen"] = routerPort + 1
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
                "intent_extractor",
                "Intent Extractor",
                true,
                20,
                new EngineModuleLaunchInfo("intent_extractor.dll"),
                []),
            new EngineModuleInfo(
                "llm_provider_qwen",
                "LLM Provider",
                true,
                30,
                new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                [],
                new GenericLlmProviderModuleOptions(4096),
                new QwenModuleOptions(19112, "qwen2.5:7b"))
        };

        var aliases = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["generic_llm_provider"] = "llm_provider_qwen"
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
