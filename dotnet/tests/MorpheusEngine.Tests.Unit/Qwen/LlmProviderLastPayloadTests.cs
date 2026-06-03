using System.Net;
using System.Net.Http.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Helpers;
using QwenProviderType = global::MorpheusEngine.LlmProviderQwen;

namespace MorpheusEngine.Tests.Unit.Qwen;

[Collection("LlmProviderListener")]
public sealed class LlmProviderLastPayloadTests : IClassFixture<LlmProviderLastPayloadHostFixture>
{
    private readonly LlmProviderLastPayloadHostFixture _fixture;

    public LlmProviderLastPayloadTests(LlmProviderLastPayloadHostFixture fixture)
    {
        _fixture = fixture;
    }

    [Fact]
    // Verifies that POST /generate captures the Ollama wire JSON for the debug endpoint.
    public async Task LastLlmPayload_AfterGenerate_ReturnsGeneratePayload()
    {
        using var generateResponse = await _fixture.Client.PostAsJsonAsync(
            "/generate",
            new LlmGenerateRequest("Say hi.", "You are helpful."));
        generateResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        using var debugResponse = await _fixture.Client.GetAsync("/debug/last_llm_payload");
        var snapshot = await debugResponse.Content.ReadFromJsonAsync<LlmProviderLastPayloadResponse>();

        snapshot.Should().NotBeNull();
        snapshot!.Available.Should().BeTrue();
        snapshot.Endpoint.Should().Be("generate");
        snapshot.PayloadJson.Should().Contain("\"prompt\"");
        snapshot.PayloadJson.Should().Contain("Say hi.");
        snapshot.PayloadJson.Should().Contain("\"model\"");
    }

    [Fact]
    // Verifies that POST /chat captures the Ollama wire JSON and overwrites a prior /generate snapshot.
    public async Task LastLlmPayload_AfterChat_ReturnsChatPayload()
    {
        using var generateResponse = await _fixture.Client.PostAsJsonAsync(
            "/generate",
            new LlmGenerateRequest("ignored.", "system"));
        generateResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        var chatRequest = new ChatGenerateRequest
        {
            Messages =
            [
                new ChatGenerateRequest.ChatMessageDto("system", "GM."),
                new ChatGenerateRequest.ChatMessageDto("user", "open door")
            ]
        };
        using var chatResponse = await _fixture.Client.PostAsJsonAsync("/chat", chatRequest);
        chatResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        using var debugResponse = await _fixture.Client.GetAsync("/debug/last_llm_payload");
        var snapshot = await debugResponse.Content.ReadFromJsonAsync<LlmProviderLastPayloadResponse>();

        snapshot.Should().NotBeNull();
        snapshot!.Endpoint.Should().Be("chat");
        snapshot.PayloadJson.Should().Contain("\"messages\"");
        snapshot.PayloadJson.Should().Contain("open door");
        snapshot.PayloadJson.Should().NotContain("ignored.");
        snapshot.PayloadJson.Should().Contain("\"think\":false");
    }
}

[CollectionDefinition("LlmProviderListener", DisableParallelization = true)]
public sealed class LlmProviderListenerCollection
{
}

public sealed class LlmProviderLastPayloadHostFixture : IAsyncLifetime
{
    private const int ProviderPort = 19081;
    private static readonly TimeSpan ListenerReadyTimeout = TimeSpan.FromSeconds(15);

    private MockOllamaHandler _ollamaHandler = null!;
    private QwenProviderType _host = null!;
    private Task _hostTask = null!;
    private CancellationTokenSource _hostCancellation = null!;

    public HttpClient Client { get; private set; } = null!;

    public async Task InitializeAsync()
    {
        var configuration = new TestConfigBuilder()
            .AddAlias("generic_llm_provider", "llm_provider_qwen")
            .AddModule(
                "llm_provider_qwen",
                ProviderPort,
                requiredByEngine: true,
                genericLlmProviderOptions: new GenericLlmProviderModuleOptions(4096),
                qwenOptions: new QwenModuleOptions(8795, "qwen2.5:7b-instruct", false),
                endpoints:
                [
                    new EngineEndpointInfo("/health", "Health", "GET", "module_health", null, null),
                    new EngineEndpointInfo("/initialize", "Initialize", "POST", "initialize", null, null),
                    new EngineEndpointInfo("/generate", "Generate", "POST", "generate", null, null),
                    new EngineEndpointInfo("/chat", "Chat", "POST", "chat", null, null),
                    new EngineEndpointInfo(
                        "/debug/last_llm_payload",
                        "Last payload",
                        "GET",
                        "llm_provider_last_llm_payload",
                        null,
                        null)
                ])
            .Build();

        _ollamaHandler = new MockOllamaHandler();
        _ollamaHandler.OnJson("GET", "/", HttpStatusCode.OK, """{"status":"ok"}""");
        _ollamaHandler.OnJson("POST", "/api/generate", HttpStatusCode.OK, """{"response":"ok","done":true}""");
        _ollamaHandler.OnJson(
            "POST",
            "/api/chat",
            HttpStatusCode.OK,
            """{"message":{"role":"assistant","content":"hi"},"done":true}""");

        _host = new QwenProviderType(
            configuration,
            new HttpClient(_ollamaHandler) { Timeout = TimeSpan.FromSeconds(10) });
        _host.DisableBundledOllamaBootstrapForTesting();
        _host.SetOllamaStateForTesting(httpReady: true, ready: true, bootstrapFailed: false);

        _hostCancellation = new CancellationTokenSource();
        _hostTask = _host.Run();
        Client = new HttpClient
        {
            BaseAddress = new Uri($"http://127.0.0.1:{ProviderPort}/"),
            Timeout = TimeSpan.FromSeconds(10)
        };

        await WaitForHealthAsync();

        using (var beforeInitResponse = await Client.GetAsync("/debug/last_llm_payload"))
        {
            beforeInitResponse.StatusCode.Should().Be(HttpStatusCode.OK);
            var beforeInit = await beforeInitResponse.Content.ReadFromJsonAsync<LlmProviderLastPayloadResponse>();
            beforeInit.Should().NotBeNull();
            beforeInit!.Available.Should().BeFalse();
        }

        using var initializeResponse = await Client.PostAsJsonAsync(
            "/initialize",
            new InitializeModuleRequest("test_game", "00000000-0000-0000-0000-000000000001"));
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
    }

    public async Task DisposeAsync()
    {
        Client.Dispose();
        _host.RequestShutdown();
        _hostCancellation.Cancel();
        try
        {
            await _hostTask.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch (OperationCanceledException)
        {
        }
        catch (TimeoutException)
        {
        }

        _hostCancellation.Dispose();
    }

    private async Task WaitForHealthAsync()
    {
        var deadline = DateTime.UtcNow + ListenerReadyTimeout;
        while (DateTime.UtcNow < deadline)
        {
            if (_hostTask.IsFaulted)
            {
                await _hostTask;
            }

            try
            {
                using var response = await Client.GetAsync("/health");
                if (response.IsSuccessStatusCode || response.StatusCode == HttpStatusCode.ServiceUnavailable)
                {
                    return;
                }
            }
            catch (HttpRequestException)
            {
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"llm_provider_qwen did not become reachable on port {ProviderPort}.");
    }
}
