using System.Net;
using System.Net.Http.Json;
using System.Text;
using MorpheusEngine;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using IntentExtractorType = global::MorpheusEngine.IntentExtractor;

namespace MorpheusEngine.Tests.Integration.IntentExtractor;

internal sealed class IntentExtractorHarness : IAsyncDisposable
{
    private static readonly TimeSpan SHUTDOWN_WAIT_TIMEOUT = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan READINESS_TIMEOUT = TimeSpan.FromSeconds(5);

    private readonly TempGameProject _gameProject;
    private readonly SingleListenerLifecycle _lifecycle;
    private readonly HttpClient _outboundHttpClient;
    private readonly IntentExtractorType _host;

    private IntentExtractorHarness(TempGameProject gameProject, EngineConfiguration configuration)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        IntentExtractorPort = configuration.GetRequiredListenPort("intent_extractor");
        RouterPort = configuration.GetRequiredListenPort("router");

        ProxyHandler = new MockRouterProxyHandler();
        _outboundHttpClient = new HttpClient(ProxyHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        _host = new IntentExtractorType(configuration, _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{IntentExtractorPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _lifecycle = new SingleListenerLifecycle(Client, _host.Run(), "IntentExtractor", IntentExtractorPort);
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
        var document =
            IntegrationEngineConfigurationFixture.LoadConfigurationsFixture("integration_intent_extractor.engine_config.json");

        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            loreCsv: null,
            systemInstructions: null);

        IntegrationEngineConfigurationFixture.WriteEngineConfigJson(gameProject.RepositoryRoot, document);
        var configurationAfterWrite =
            IntegrationEngineConfigurationFixture.LoadConfigurationViaEngineConfigLoader(gameProject.RepositoryRoot);
        var harness = new IntentExtractorHarness(gameProject, configurationAfterWrite);
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
        var collector = new HarnessTeardownErrorCollector(nameof(IntentExtractorHarness));
        await collector.RunAsync(
            "intent_extractor.shutdown",
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
