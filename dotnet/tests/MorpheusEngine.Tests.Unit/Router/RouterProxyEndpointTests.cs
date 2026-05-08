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
public sealed class RouterProxyEndpointTests
{
    // Verifies that allowlisted proxy requests are forwarded and return the downstream response.
    [Fact]
    public async Task Router_PostProxy_ValidAllowlistedPair_ForwardsAndReturnsResponse()
    {
        var downstreamHandler = new MockHttpHandler();
        string? forwardedBody = null;
        downstreamHandler.On(
            "POST",
            "/message",
            request =>
            {
                forwardedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent("""{"ok":true,"text":"Director response."}""", Encoding.UTF8, "application/json")
                };
            });

        await using var harness = await RouterProxyHarness.StartAsync(downstreamHandler);

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "memory_director",
                  "targetPath": "/message",
                  "method": "POST",
                  "body": {
                    "turn": 1,
                    "playerInput": "look around"
                  }
                }
                """));

        var payload = await ReadJsonAsync<DirectorMessageResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        payload.Should().Be(new DirectorMessageResponse(true, "Director response."));
        downstreamHandler.SentRequests.Should().HaveCount(1);
        downstreamHandler.SentRequests[0].Method.Method.Should().Be("POST");
        downstreamHandler.SentRequests[0].RequestUri!.AbsolutePath.Should().Be("/message");
        using var forwardedDocument = JsonDocument.Parse(forwardedBody!);
        forwardedDocument.RootElement.GetProperty("turn").GetInt32().Should().Be(1);
        forwardedDocument.RootElement.GetProperty("playerInput").GetString().Should().Be("look around");
    }

    // Verifies that proxy requests reject target paths outside the allowlist.
    [Fact]
    public async Task Router_PostProxy_TargetPathNotAllowlisted_Returns403()
    {
        await using var harness = await RouterProxyHarness.StartAsync(new MockHttpHandler());

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "memory_director",
                  "targetPath": "/not-allowlisted",
                  "method": "POST",
                  "body": {
                    "turn": 1
                  }
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.Forbidden);
        payload.Error.Should().Contain("not allowed");
    }

    // Verifies that proxy requests reject unknown target modules.
    [Fact]
    public async Task Router_PostProxy_UnknownTargetModule_Returns400()
    {
        await using var harness = await RouterProxyHarness.StartAsync(new MockHttpHandler());

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "missing_module",
                  "targetPath": "/message",
                  "method": "POST",
                  "body": {
                    "turn": 1
                  }
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("Unknown target module");
    }

    // Verifies that proxy requests require a non-empty source module.
    [Fact]
    public async Task Router_PostProxy_EmptySourceModule_Returns400()
    {
        await using var harness = await RouterProxyHarness.StartAsync(new MockHttpHandler());

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "   ",
                  "targetModule": "memory_director",
                  "targetPath": "/message",
                  "method": "POST",
                  "body": {
                    "turn": 1
                  }
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("sourceModule");
    }

    // Verifies that proxy requests reject unsupported HTTP methods.
    [Fact]
    public async Task Router_PostProxy_UnsupportedMethod_Returns400()
    {
        await using var harness = await RouterProxyHarness.StartAsync(new MockHttpHandler());

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "memory_director",
                  "targetPath": "/message",
                  "method": "DELETE"
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        payload.Error.Should().Contain("Unsupported proxy method");
    }

    // Verifies that non-JSON downstream responses surface as router errors.
    [Fact]
    public async Task Router_PostProxy_DownstreamNonJsonContentType_Returns500()
    {
        var downstreamHandler = new MockHttpHandler();
        downstreamHandler.On(
            "POST",
            "/message",
            _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("plain text response", Encoding.UTF8, "text/plain")
            });

        await using var harness = await RouterProxyHarness.StartAsync(downstreamHandler);

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "memory_director",
                  "targetPath": "/message",
                  "method": "POST",
                  "body": {
                    "turn": 1,
                    "playerInput": "look around"
                  }
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.InternalServerError);
        payload.Error.Should().Be("Unhandled router error.");
        payload.Details.Should().Contain("Content-Type 'application/json'");
    }

    // Verifies that unreachable downstream modules return a bad gateway error.
    [Fact]
    public async Task Router_PostProxy_DownstreamUnreachable_Returns502()
    {
        var downstreamHandler = new MockHttpHandler();
        downstreamHandler.On("POST", "/message", _ => throw new HttpRequestException("connection refused"));

        await using var harness = await RouterProxyHarness.StartAsync(downstreamHandler);

        using var response = await harness.Client.PostAsync(
            "/proxy",
            JsonContent(
                """
                {
                  "sourceModule": "intent_extractor",
                  "targetModule": "memory_director",
                  "targetPath": "/message",
                  "method": "POST",
                  "body": {
                    "turn": 1,
                    "playerInput": "look around"
                  }
                }
                """));

        var payload = await ReadJsonAsync<ErrorResponse>(response);

        response.StatusCode.Should().Be(HttpStatusCode.BadGateway);
        payload.Error.Should().Contain("Failed to reach target module");
        payload.Details.Should().Contain("connection refused");
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

    private sealed class RouterProxyHarness : IAsyncDisposable
    {
        private readonly HttpClient _client;
        private readonly Task _runTask;
        private readonly TempRouterRepository _repository;

        private RouterProxyHarness(HttpClient client, Task runTask, TempRouterRepository repository)
        {
            _client = client;
            _runTask = runTask;
            _repository = repository;
        }

        public HttpClient Client => _client;

        public static async Task<RouterProxyHarness> StartAsync(MockHttpHandler downstreamHandler)
        {
            var port = AllocateFreeTcpPort();
            var repository = new TempRouterRepository();
            var configuration = BuildConfiguration(repository.RepositoryRoot, port);
            var router = new RouterType(configuration, new HttpClient(downstreamHandler));
            var runTask = Task.Run(() => router.Run());
            var client = new HttpClient
            {
                BaseAddress = new Uri($"http://127.0.0.1:{port}/"),
                Timeout = TimeSpan.FromSeconds(5)
            };

            var harness = new RouterProxyHarness(client, runTask, repository);
            await harness.WaitUntilReadyAsync();
            return harness;
        }

        public async ValueTask DisposeAsync()
        {
            if (!_runTask.IsCompleted)
            {
                try
                {
                    using var _ = await _client.PostAsync("/shutdown", JsonContent("""{}"""));
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
                    var completed = await Task.WhenAny(_runTask, Task.Delay(TimeSpan.FromSeconds(5)));
                    completed.Should().Be(_runTask, "router should stop promptly during test cleanup");
                }

                await _runTask;
            }
            finally
            {
                _client.Dispose();
                _repository.Dispose();
            }
        }

        private async Task WaitUntilReadyAsync()
        {
            var deadline = DateTime.UtcNow.AddSeconds(5);

            while (DateTime.UtcNow < deadline)
            {
                if (_runTask.IsCompleted)
                {
                    await _runTask;
                }

                try
                {
                    using var response = await _client.GetAsync("/info");
                    if (response.StatusCode == HttpStatusCode.OK)
                    {
                        return;
                    }
                }
                catch
                {
                }

                await Task.Delay(50);
            }

            throw new TimeoutException("Router proxy harness did not become ready in time.");
        }

        private static EngineConfiguration BuildConfiguration(string repositoryRoot, int routerPort)
        {
            return new TestConfigBuilder(repositoryRoot)
                .AddAlias("generic_llm_provider", "llm_provider_qwen")
                .AddAlias("generic_director", "memory_director")
                .AddAlias("generic_embeddings", "embeddings_ollama")
                .AddModule(
                    "router",
                    routerPort,
                    requiredByEngine: true,
                    endpoints:
                    [
                        new EngineEndpointInfo("/info", "Info", "GET", "module_info", null, null),
                        new EngineEndpointInfo("/health", "Health", "GET", "module_health", null, null),
                        new EngineEndpointInfo("/shutdown", "Shutdown", "POST", "module_shutdown", null, null),
                        new EngineEndpointInfo("/initialize", "Initialize", "POST", "initialize", null, null),
                        new EngineEndpointInfo("/turn", "Turn", "POST", "turn", null, null),
                        new EngineEndpointInfo("/proxy", "Proxy", "POST", "module_proxy", null, null)
                    ])
                .AddModule(
                    "memory_director",
                    routerPort + 1,
                    requiredByEngine: true,
                    endpoints:
                    [
                        new EngineEndpointInfo("/message", "Message", "POST", "director_message", null, null)
                    ])
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
                            "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}}}")
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
            RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_router_proxy_" + Guid.NewGuid().ToString("N"));
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
