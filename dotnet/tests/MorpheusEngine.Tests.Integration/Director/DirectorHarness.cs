using System.Net;
using System.Net.Http.Json;
using System.Text;
using MorpheusEngine;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using DirectorType = global::MorpheusEngine.Director;

namespace MorpheusEngine.Tests.Integration.Director;

internal sealed class DirectorHarness : IAsyncDisposable
{
    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly DirectorType _host;

    private DirectorHarness(TempGameProject gameProject, EngineConfiguration configuration)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";

        RouterPort = configuration.GetRequiredListenPort("router");
        DirectorPort = configuration.GetRequiredListenPort("director");

        ProxyHandler = new MockRouterProxyHandler();
        _outboundHttpClient = new HttpClient(ProxyHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        _host = new DirectorType(configuration, _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{DirectorPort}/"),
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
        var configDocument = IntegrationEngineConfigurationFixture.LoadConfigurationsFixture("integration_director.engine_config.json");

        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);

        IntegrationEngineConfigurationFixture.WriteEngineConfigJson(gameProject.RepositoryRoot, configDocument);
        WriteDirectorSchema(gameProject);

        var configuration =
            IntegrationEngineConfigurationFixture.LoadConfigurationViaEngineConfigLoader(gameProject.RepositoryRoot);

        var harness = new DirectorHarness(gameProject, configuration);
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

    private static void WriteDirectorSchema(TempGameProject gameProject)
    {
        var schemaDirectory = Path.Combine(
            gameProject.RepositoryRoot,
            "dotnet",
            "src",
            "MorpheusEngine.LlmProvider_qwen",
            "schemas");
        Directory.CreateDirectory(schemaDirectory);
        File.WriteAllText(
            Path.Combine(schemaDirectory, "director_action.schema.json"),
            File.ReadAllText(
                Path.Combine(
                    GetRepositoryRoot(),
                    "dotnet",
                    "src",
                    "MorpheusEngine.LlmProvider_qwen",
                    "schemas",
                    "director_action.schema.json")));
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
