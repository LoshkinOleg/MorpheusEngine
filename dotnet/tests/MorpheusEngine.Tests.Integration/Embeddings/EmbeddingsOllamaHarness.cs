using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.Embeddings;

internal sealed class EmbeddingsOllamaHarness : IAsyncDisposable
{
    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly EmbeddingsOllamaModule _host;

    private EmbeddingsOllamaHarness(TempGameProject gameProject, int embeddingsPort, int ollamaPort)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        EmbeddingsPort = embeddingsPort;
        OllamaPort = ollamaPort;

        OllamaHandler = new MockOllamaHandler();
        _outboundHttpClient = new HttpClient(OllamaHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _host = new EmbeddingsOllamaModule(CreateConfiguration(RepositoryRoot, embeddingsPort, ollamaPort), _outboundHttpClient);
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{embeddingsPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _runTask = _host.RunAsync();
    }

    public HttpClient Client { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public int EmbeddingsPort { get; }

    public int OllamaPort { get; }

    public MockOllamaHandler OllamaHandler { get; }

    public static async Task<EmbeddingsOllamaHarness> CreateAsync()
    {
        var embeddingsPort = GetFreeTcpPort();
        var ollamaPort = GetFreeTcpPort();
        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);
        var harness = new EmbeddingsOllamaHarness(gameProject, embeddingsPort, ollamaPort);
        await harness.WaitUntilReadyAsync();
        return harness;
    }

    public Task<HttpResponseMessage> InitializeAsync(string? gameProjectId = null, string? runId = null)
    {
        return Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest(gameProjectId ?? GameProjectId, runId ?? RunId));
    }

    public Task<HttpResponseMessage> PostEmbedAsync(IReadOnlyList<string> texts, string model = "")
    {
        return Client.PostAsJsonAsync("/embed", new EmbeddingRequest(model, texts));
    }

    public Task<HttpResponseMessage> PostTokenCountAsync(string text, string model = "")
    {
        return Client.PostAsJsonAsync("/token_count", new TokenCountRequest(model, text));
    }

    public Task<HttpResponseMessage> GetHealthAsync()
    {
        return Client.GetAsync("/health");
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
            // Best-effort shutdown for temporary integration hosts.
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

        throw new TimeoutException($"EmbeddingsOllamaModule did not start listening on port {EmbeddingsPort} within the allotted time.");
    }

    private static EngineConfiguration CreateConfiguration(string repositoryRoot, int embeddingsPort, int ollamaPort)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["embeddings_ollama"] = embeddingsPort
        };

        var modules = new[]
        {
            new EngineModuleInfo(
                "embeddings_ollama",
                "Embeddings",
                true,
                10,
                new EngineModuleLaunchInfo("embeddings_ollama.dll"),
                [],
                null,
                null,
                null,
                new EmbeddingsModuleOptions(ollamaPort, "nomic-embed-text", "30m", 2048))
        };

        var aliases = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
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
