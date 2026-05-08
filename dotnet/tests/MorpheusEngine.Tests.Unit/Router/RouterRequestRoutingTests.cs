using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Fixtures;
using MorpheusEngine.Tests.Unit.Helpers;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterRequestRoutingTests
{
    [Fact]
    public async Task Router_GetInfo_Returns200WithRouterModuleName()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/info");
        var payload = await ReadJsonAsync<ModuleInfoResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleInfoResponse(true, "router"));
    }

    [Fact]
    public async Task Router_GetHealth_BeforeBind_ReturnsAwaitingInitialize()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/health");
        var payload = await ReadJsonAsync<ModuleHealthResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleHealthResponse(false, "awaiting_initialize", false));
    }

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

    [Fact]
    public async Task Router_PostShutdown_Returns200AndSetsShutdownRequested()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.PostAsync("/shutdown", JsonContent("""{}"""));
        var payload = await ReadJsonAsync<ModuleShutdownResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new ModuleShutdownResponse(true, "Shutdown requested."));
        await harness.AwaitStoppedAsync();
        harness.GetPrivateField<bool>("_shutdownRequested").Should().BeTrue();
    }

    [Fact]
    public async Task Router_UnknownPath_Returns404()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/missing");
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.NotFound);
        payload.Error.Should().Contain("Not found");
    }

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
        harness.GetPrivateField<bool>("_runBound").Should().BeTrue();
        harness.GetPrivateField<string>("_boundGameProjectId").Should().Be("test_game");
        harness.GetPrivateField<string>("_boundRunId").Should().Be("run-001");
        harness.GetPrivateField<EngineTurnPipelineInfo>("_turnPipeline").Id.Should().Be("memory_director_default");
    }

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

    [Fact]
    public async Task Router_GetInitialize_Returns405()
    {
        await using var harness = await RouterHarness.StartAsync();

        using var response = await harness.Client.GetAsync("/initialize");
        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.MethodNotAllowed);
        payload.Error.Should().Contain("Method not allowed");
    }

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

        private RouterHarness(HttpClient httpClient, RouterType router, Task runTask, TempRouterRepository repository)
        {
            _httpClient = httpClient;
            Router = router;
            _runTask = runTask;
            _repository = repository;
        }

        public RouterType Router { get; }

        public HttpClient Client => _httpClient;

        public static async Task<RouterHarness> StartAsync()
        {
            var port = AllocateFreeTcpPort();
            var repository = new TempRouterRepository();
            var configuration = BuildConfiguration(repository.RepositoryRoot, port);
            var httpClient = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{port}/"),
                Timeout = TimeSpan.FromSeconds(5)
            };
            var router = new RouterType(configuration, new HttpClient(new MockHttpHandler()));
            var runTask = Task.Run(() => router.Run());
            var harness = new RouterHarness(httpClient, router, runTask, repository);
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

        public T GetPrivateField<T>(string fieldName)
        {
            var field = typeof(RouterType).GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
            field.Should().NotBeNull($"expected private field '{fieldName}' to exist");
            return (T)field!.GetValue(Router)!;
        }

        public async Task AwaitStoppedAsync()
        {
            var completed = await Task.WhenAny(_runTask, Task.Delay(TimeSpan.FromSeconds(5)));
            completed.Should().Be(_runTask, "router should stop promptly after /shutdown");
            await _runTask;
        }

        public async ValueTask DisposeAsync()
        {
            if (!_runTask.IsCompleted)
            {
                try
                {
                    using var _ = await _httpClient.PostAsync("/shutdown", JsonContent("""{}"""));
                }
                catch
                {
                    // Best effort if the listener is already stopping.
                }
            }

            try
            {
                if (!_runTask.IsCompleted)
                {
                    await AwaitStoppedAsync();
                }
                else
                {
                    await _runTask;
                }
            }
            finally
            {
                _httpClient.Dispose();
                _repository.Dispose();
            }
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

        private static int AllocateFreeTcpPort()
        {
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
    }

    private sealed class TempRouterRepository : IDisposable
    {
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
            try
            {
                if (Directory.Exists(RepositoryRoot))
                {
                    Directory.Delete(RepositoryRoot, recursive: true);
                }
            }
            catch
            {
                // Best-effort cleanup for temp router repositories.
            }
        }
    }
}
