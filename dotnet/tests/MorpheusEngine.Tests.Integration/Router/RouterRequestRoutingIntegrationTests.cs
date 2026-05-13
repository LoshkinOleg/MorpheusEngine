using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Integration.Router;

[Collection("EngineProcessState")]
[Trait("Category", "Integration")]
public sealed class RouterRequestRoutingIntegrationTests
{
    // Verifies that the info endpoint returns the router module metadata.
    [Fact]
    public async Task Router_GetInfo_Returns200WithRouterModuleName()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/info");
        var payload = await ReadJsonAsync<ModuleInfoResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleInfoResponse(true, "router"));
    }

    // Verifies that health reports awaiting initialization before binding a run.
    [Fact]
    public async Task Router_GetHealth_BeforeBind_ReturnsAwaitingInitialize()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/health");
        var payload = await ReadJsonAsync<ModuleHealthResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleHealthResponse(false, "awaiting_initialize", false));
    }

    // Verifies that health reports healthy after a run is bound.
    [Fact]
    public async Task Router_GetHealth_AfterBind_ReturnsHealthy()
    {
        await using var harness = await RouterHarness.StartAsync();
        await harness.InitializeAsync();

        using var response = await harness.Client.GetAsync("/health");
        var payload = await ReadJsonAsync<ModuleHealthResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleHealthResponse(true, "healthy", true));
    }

    // Verifies that shutdown succeeds and marks the router as stopping.
    [Fact]
    public async Task Router_PostShutdown_Returns200AndSetsShutdownRequested()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync("/shutdown", JsonContent("""{}"""));
        var payload = await ReadJsonAsync<ModuleShutdownResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleShutdownResponse(true, "Shutdown requested."));
        await harness.AwaitStoppedAsync();
    }

    // Verifies that unknown routes return a not found error.
    [Fact]
    public async Task Router_UnknownPath_Returns404()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/missing");
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.NotFound);
        payload.Error.Should().Contain("Not found");
    }

    // Verifies that initialize binds the run and loads the default pipeline.
    [Fact]
    public async Task Router_PostInitialize_ValidPayload_BindsRun()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync(
            "/initialize",
            JsonContent("""{"gameProjectId":"test_game","runId":"run-001"}"""));
        var payload = await ReadJsonAsync<InitializeModuleResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new InitializeModuleResponse(true));
        using var healthResponse = await harness.Client.GetAsync("/health");
        var healthPayload = await ReadJsonAsync<ModuleHealthResponse>(healthResponse);
        healthResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        healthPayload.Should().Be(new ModuleHealthResponse(true, "healthy", true));
    }

    // Verifies that initialize rejects requests missing the run ID.
    [Fact]
    public async Task Router_PostInitialize_MissingRunId_Returns400()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync(
            "/initialize",
            JsonContent("""{"gameProjectId":"test_game"}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("runId");
    }

    // Verifies that initialize rejects requests missing the game project ID.
    [Fact]
    public async Task Router_PostInitialize_MissingGameProjectId_Returns400()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync(
            "/initialize",
            JsonContent("""{"runId":"run-001"}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("gameProjectId");
    }

    // Verifies that initialize rejects rebinding an already bound run.
    [Fact]
    public async Task Router_PostInitialize_WhenAlreadyBound_Returns409()
    {
        await using var harness = await RouterHarness.StartAsync();
        await harness.InitializeAsync();

        using var response = await harness.Client.PostAsync(
            "/initialize",
            JsonContent("""{"gameProjectId":"test_game","runId":"run-002"}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.Conflict);
        payload.Error.Should().Contain("already bound");
    }

    // Verifies that initialize only accepts POST requests.
    [Fact]
    public async Task Router_GetInitialize_Returns405()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/initialize");
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.MethodNotAllowed);
        payload.Error.Should().Contain("Method not allowed");
    }

    // Verifies that turn requests are unavailable before initialization.
    [Fact]
    public async Task Router_PostTurn_BeforeBind_Returns503()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync(
            "/turn",
            JsonContent("""{"turn":1,"playerInput":"look around"}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.ServiceUnavailable);
        payload.Error.Should().Contain("not bound");
    }

    // Verifies that turn requests reject turn numbers below one.
    [Fact]
    public async Task Router_PostTurn_TurnLessThanOne_Returns400()
    {
        await using var harness = await RouterHarness.StartAsync();
        await harness.InitializeAsync();

        using var response = await harness.Client.PostAsync(
            "/turn",
            JsonContent("""{"turn":0,"playerInput":"look around"}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("Turn must be >= 1");
    }

    // Verifies that turn requests reject blank player input.
    [Fact]
    public async Task Router_PostTurn_EmptyPlayerInput_Returns400()
    {
        await using var harness = await RouterHarness.StartAsync();
        await harness.InitializeAsync();

        using var response = await harness.Client.PostAsync(
            "/turn",
            JsonContent("""{"turn":1,"playerInput":"   "}"""));
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("non-empty playerInput");
    }

    private static StringContent JsonContent(string json)
    {
        return new StringContent(json, Encoding.UTF8, "application/json");
    }

    private static async Task<T> ReadJsonAsync<T>(HttpResponseMessage response)
    {
        var body = await response.Content.ReadAsStringAsync();
        var payload = JsonSerializer.Deserialize<T>(body, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });

        payload.Should().NotBeNull($"expected {typeof(T).Name} JSON but got '{body}'");
        return payload!;
    }

    private sealed class RouterHarness : IAsyncDisposable
    {
        private readonly HttpClient _httpClient;
        private readonly Task _runTask;
        private readonly TempRouterRepository _repository;

        private RouterHarness(HttpClient httpClient, Task runTask, TempRouterRepository repository)
        {
            _httpClient = httpClient;
            _runTask = runTask;
            _repository = repository;
        }

        public HttpClient Client => _httpClient;

        public static async Task<RouterHarness> StartAsync()
        {
            var port = GetFreeTcpPort();
            var repository = new TempRouterRepository();
            var configuration = BuildConfiguration(repository.RepositoryRoot, port);
            var httpClient = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{port}/"),
                Timeout = TimeSpan.FromSeconds(5)
            };
            var router = new RouterType(configuration, new HttpClient(new MockHttpHandler()));
            var runTask = Task.Run(() => router.Run());
            var harness = new RouterHarness(httpClient, runTask, repository);
            await harness.WaitUntilReadyAsync();
            return harness;
        }

        public async Task InitializeAsync(string runId = "run-001")
        {
            using var response = await _httpClient.PostAsync(
                "/initialize",
                JsonContent($$"""{"gameProjectId":"test_game","runId":"{{runId}}"}"""));

            response.StatusCode.Should().Be(HttpStatusCode.OK);
        }

        public async Task AwaitStoppedAsync()
        {
            var completed = await Task.WhenAny(_runTask, Task.Delay(TimeSpan.FromSeconds(5)));
            completed.Should().Be(_runTask, "router should stop promptly after /shutdown");
            await _runTask;
        }

        public async ValueTask DisposeAsync()
        {
            var teardown = new HarnessTeardownErrorCollector(nameof(RouterHarness));
            if (!_runTask.IsCompleted)
            {
                await teardown.RunAsync(
                    "request /shutdown",
                    async () =>
                {
                    using var _ = await _httpClient.PostAsync("/shutdown", JsonContent("""{}"""));
                });
            }

            await teardown.RunAsync(
                "await router run task",
                async () =>
            {
                if (!_runTask.IsCompleted)
                {
                    await AwaitStoppedAsync();
                }
                else
                {
                    await _runTask;
                }
            });
            teardown.Run("dispose http client", _httpClient.Dispose);
            teardown.Run("dispose temp repository", _repository.Dispose);
            teardown.ThrowIfAny();
        }

        private async Task WaitUntilReadyAsync()
        {
            var deadline = DateTime.UtcNow.AddSeconds(5);
            Exception? lastError = null;

            while (DateTime.UtcNow < deadline)
            {
                if (_runTask.IsCompleted)
                {
                    await _runTask;
                }

                try
                {
                    using var response = await _httpClient.GetAsync("/info");
                    if (response.StatusCode == HttpStatusCode.OK)
                    {
                        return;
                    }
                }
                catch (Exception e)
                {
                    lastError = e;
                }

                await Task.Delay(50);
            }

            throw new TimeoutException("Router did not become ready in time.", lastError);
        }

        private static EngineConfiguration BuildConfiguration(string repositoryRoot, int routerPort)
        {
            return new TestConfigBuilder(repositoryRoot)
                .AddAlias("generic_llm_provider", "llm_provider_qwen")
                .AddAlias("generic_director", "memory_director")
                .AddAlias("generic_embeddings", "embeddings_ollama")
                .AddModule("router", routerPort, requiredByEngine: true)
                .AddModule("memory_director", routerPort + 1, requiredByEngine: true)
                .AddModule("llm_provider_qwen", routerPort + 2, requiredByEngine: true)
                .AddModule("embeddings_ollama", routerPort + 3, requiredByEngine: true)
                .AddModule("session_store", routerPort + 4)
                .AddPipeline(
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
                    "director_message",
                    "director_message_response")
                .Build();
        }

        // Temporary helper with an acknowledged TOCTOU race: the port is reserved by binding to
        // 0, then released, then rebound by the module listener. This is tolerated in Phase 3
        // because these tests now run in the sequential integration lane. Phase 4 should replace
        // this with a deeper design that avoids bind-then-rebind windows.
        private static int GetFreeTcpPort()
        {
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
    }

    private sealed class TempRouterRepository : IDisposable
    {
        private const int MAX_DELETE_ATTEMPTS = 4;
        private static readonly int[] DELETE_BACKOFF_MS = [25, 75, 150];

        public string RepositoryRoot { get; }

        public TempRouterRepository()
        {
            RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_router_" + Guid.NewGuid().ToString("N"));
            var gameProjectDirectory = Path.Combine(RepositoryRoot, "game_projects", "test_game");
            Directory.CreateDirectory(Path.Combine(RepositoryRoot, "dotnet"));
            Directory.CreateDirectory(gameProjectDirectory);
            File.WriteAllText(Path.Combine(RepositoryRoot, "dotnet", "MorpheusEngine.sln"), string.Empty);
            File.WriteAllText(Path.Combine(gameProjectDirectory, "manifest.json"), TestPayloads.MinimalManifestJson);
        }

        public void Dispose()
        {
            if (!Directory.Exists(RepositoryRoot))
            {
                return;
            }

            Exception? lastException = null;
            for (var attempt = 0; attempt < MAX_DELETE_ATTEMPTS; attempt++)
            {
                try
                {
                    Directory.Delete(RepositoryRoot, recursive: true);
                    return;
                }
                catch (IOException ex)
                {
                    // Listener shutdown and file-indexing races can hold handles briefly after test completion.
                    lastException = ex;
                }
                catch (UnauthorizedAccessException ex)
                {
                    // Windows may transiently deny recursive delete while AV/indexers inspect new files.
                    lastException = ex;
                }

                if (attempt < MAX_DELETE_ATTEMPTS - 1)
                {
                    Thread.Sleep(DELETE_BACKOFF_MS[attempt]);
                }
            }

            throw lastException
                ?? new InvalidOperationException(
                    $"TempRouterRepository failed to delete '{RepositoryRoot}' after {MAX_DELETE_ATTEMPTS} attempts.");
        }
    }
}
