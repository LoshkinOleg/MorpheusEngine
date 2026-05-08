using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using QwenProviderType = global::MorpheusEngine.LlmProviderQwen;

namespace MorpheusEngine.Tests.Integration.Qwen;

internal sealed class LlmProviderQwenHarness : IAsyncDisposable
{
    private readonly TempGameProject _gameProject;
    private readonly Task _runTask;
    private readonly HttpClient _outboundHttpClient;
    private readonly QwenProviderType _host;

    private LlmProviderQwenHarness(TempGameProject gameProject, int providerPort, int ollamaPort)
    {
        _gameProject = gameProject;
        RepositoryRoot = gameProject.RepositoryRoot;
        GameProjectId = gameProject.GameProjectId;
        RunId = "test_run_001";
        ProviderPort = providerPort;
        OllamaPort = ollamaPort;

        OllamaHandler = new MockOllamaHandler();
        _outboundHttpClient = new HttpClient(OllamaHandler)
        {
            Timeout = TimeSpan.FromSeconds(10)
        };
        _host = new QwenProviderType(CreateConfiguration(RepositoryRoot, providerPort, ollamaPort), _outboundHttpClient);
        _host.DisableBundledOllamaBootstrapForTesting();
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{providerPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };
        _runTask = _host.Run();
    }

    public HttpClient Client { get; }

    public string RepositoryRoot { get; }

    public string GameProjectId { get; }

    public string RunId { get; }

    public int ProviderPort { get; }

    public int OllamaPort { get; }

    public MockOllamaHandler OllamaHandler { get; }

    public static async Task<LlmProviderQwenHarness> CreateAsync()
    {
        var providerPort = GetFreeTcpPort();
        var ollamaPort = GetFreeTcpPort();
        var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions);
        var harness = new LlmProviderQwenHarness(gameProject, providerPort, ollamaPort);
        await harness.WaitUntilReadyAsync();
        return harness;
    }

    public void SetOllamaState(bool httpReady, bool ready, bool bootstrapFailed)
    {
        _host.SetOllamaStateForTesting(httpReady, ready, bootstrapFailed);
    }

    public Task<HttpResponseMessage> InitializeAsync(string? gameProjectId = null, string? runId = null)
    {
        return Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest(gameProjectId ?? GameProjectId, runId ?? RunId));
    }

    public Task<HttpResponseMessage> PostGenerateAsync(string prompt, string system = "You are a helpful assistant.")
    {
        return Client.PostAsJsonAsync("/generate", new LlmGenerateRequest(prompt, system));
    }

    public Task<HttpResponseMessage> PostChatAsync(ChatGenerateRequest request)
    {
        return Client.PostAsJsonAsync("/chat", request);
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

        throw new TimeoutException($"LlmProviderQwen did not start listening on port {ProviderPort} within the allotted time.");
    }

    private static EngineConfiguration CreateConfiguration(string repositoryRoot, int providerPort, int ollamaPort)
    {
        var ports = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["llm_provider_qwen"] = providerPort
        };

        var modules = new[]
        {
            new EngineModuleInfo(
                "llm_provider_qwen",
                "LLM Provider",
                true,
                10,
                new EngineModuleLaunchInfo("llm_provider_qwen.dll"),
                [],
                new GenericLlmProviderModuleOptions(4096),
                new QwenModuleOptions(ollamaPort, "qwen2.5:7b"))
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
