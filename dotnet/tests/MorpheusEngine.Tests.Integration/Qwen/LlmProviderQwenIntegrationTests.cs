using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Integration.Qwen;

[Trait("Category", "Integration")]
public sealed class LlmProviderQwenIntegrationTests
{
    [Fact]
    public async Task LlmProviderQwen_PostChat_ValidMessages_ReturnsAssistantText()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        ConfigureHealthyOllamaForInitialize(harness, """{"response":"primed","done":true}""");

        harness.OllamaHandler.OnJson(
            "POST",
            "/api/chat",
            HttpStatusCode.OK,
            """{"message":{"role":"assistant","content":"The vault door yields with a metallic sigh."},"done":true}""");

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
        await WaitForHealthStatusAsync(harness, "healthy");

        var request = new ChatGenerateRequest
        {
            Messages =
            [
                new ChatGenerateRequest.ChatMessageDto("system", "Be concise."),
                new ChatGenerateRequest.ChatMessageDto("user", "open the vault")
            ]
        };
        using var response = await harness.PostChatAsync(request);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<ChatGenerateResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Response.Should().Be("The vault door yields with a metallic sigh.");

        var captured = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/chat");
        using var document = JsonDocument.Parse(captured.Body);
        document.RootElement.GetProperty("model").GetString().Should().Be("qwen2.5:7b");
        document.RootElement.GetProperty("messages")[1].GetProperty("content").GetString().Should().Be("open the vault");
        document.RootElement.GetProperty("options").GetProperty("num_ctx").GetInt32().Should().Be(4096);
    }

    [Fact]
    public async Task LlmProviderQwen_PostChat_WithFormat_ForwardsSchemaAndKeepAlive()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        ConfigureHealthyOllamaForInitialize(harness, """{"response":"primed","done":true}""");

        harness.OllamaHandler.OnJson(
            "POST",
            "/api/chat",
            HttpStatusCode.OK,
            """{"message":{"role":"assistant","content":"{\"intent\":\"inspect\"}"},"done":true}""");

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
        await WaitForHealthStatusAsync(harness, "healthy");

        var format = ParseJsonElement(
            """
            {
              "type": "object",
              "properties": {
                "intent": { "type": "string" }
              },
              "required": ["intent"]
            }
            """);
        var request = new ChatGenerateRequest
        {
            Messages = [new ChatGenerateRequest.ChatMessageDto("user", "inspect the room")],
            Format = format,
            KeepAlive = "10m"
        };

        using var response = await harness.PostChatAsync(request);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<ChatGenerateResponse>();
        payload.Should().NotBeNull();
        payload!.Response.Should().Be("""{"intent":"inspect"}""");

        var captured = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/chat");
        using var document = JsonDocument.Parse(captured.Body);
        document.RootElement.GetProperty("format").GetProperty("properties").GetProperty("intent").GetProperty("type").GetString().Should().Be("string");
        document.RootElement.GetProperty("keep_alive").GetString().Should().Be("10m");
    }

    [Fact]
    public async Task LlmProviderQwen_PostGenerate_ValidPrompt_ReturnsGeneratedText()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        var generateResponses = new Queue<string>(
        [
            """{"response":"primed","done":true}""",
            """{"response":"A pale light spills into the corridor.","done":true}"""
        ]);
        ConfigureQueuedGenerateResponses(harness, generateResponses);

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
        await WaitForHealthStatusAsync(harness, "healthy");

        using var response = await harness.PostGenerateAsync("describe the corridor", "You are terse.");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<LlmProviderGenerateResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Response.Should().Be("A pale light spills into the corridor.");

        var generateRequest = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/generate");
        using var document = JsonDocument.Parse(generateRequest.Body);
        document.RootElement.GetProperty("prompt").GetString().Should().Be("describe the corridor");
        document.RootElement.GetProperty("system").GetString().Should().Be("You are terse.");
        document.RootElement.GetProperty("model").GetString().Should().Be("qwen2.5:7b");
    }

    [Fact]
    public async Task LlmProviderQwen_PostTokenCount_ReturnsPromptEvalCount()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        var generateResponses = new Queue<string>(
        [
            """{"response":"primed","done":true}""",
            """{"prompt_eval_count":23,"done":true}"""
        ]);
        ConfigureQueuedGenerateResponses(harness, generateResponses);

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
        await WaitForHealthStatusAsync(harness, "healthy");

        using var response = await harness.PostTokenCountAsync("count this prompt");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<TokenCountResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Model.Should().Be("qwen2.5:7b");
        payload.EstimatedTokens.Should().Be(23);
        payload.Exact.Should().BeTrue();

        var tokenCountRequest = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/generate");
        using var document = JsonDocument.Parse(tokenCountRequest.Body);
        document.RootElement.GetProperty("prompt").GetString().Should().Be("count this prompt");
        document.RootElement.GetProperty("options").GetProperty("num_predict").GetInt32().Should().Be(0);
    }

    [Fact]
    public async Task LlmProviderQwen_Health_DuringBootstrap_ReturnsOllamaStarting()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();

        await WaitForHealthStatusAsync(harness, "ollama_starting");
    }

    [Fact]
    public async Task LlmProviderQwen_Health_AfterBootstrapFailure_ReturnsOllamaStartupFailed()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        harness.SetOllamaState(httpReady: false, ready: false, bootstrapFailed: true);

        await WaitForHealthStatusAsync(harness, "ollama_startup_failed");
    }

    [Fact]
    public async Task LlmProviderQwen_PostInitialize_ModelAlreadyPresent_DoesNotRedownloadModel()
    {
        await using var harness = await LlmProviderQwenHarness.CreateAsync();
        ConfigureHealthyOllamaForInitialize(harness, """{"response":"primed","done":true}""");

        using var initializeResponse = await harness.InitializeAsync();

        initializeResponse.StatusCode.Should().Be(HttpStatusCode.Accepted);
        await WaitForHealthStatusAsync(harness, "healthy");

        harness.OllamaHandler.CapturedRequests.Select(request => request.Path).Should().ContainSingle(path => path == "/api/generate");
        harness.OllamaHandler.CapturedRequests.Select(request => request.Path).Should().NotContain("/api/pull");
        harness.OllamaHandler.CapturedRequests.Select(request => request.Path).Should().NotContain("/api/show");
        harness.OllamaHandler.CapturedRequests.Select(request => request.Path).Should().NotContain("/api/tags");
    }

    private static void ConfigureHealthyOllamaForInitialize(LlmProviderQwenHarness harness, string primingResponseJson)
    {
        harness.SetOllamaState(httpReady: true, ready: false, bootstrapFailed: false);
        harness.OllamaHandler.OnJson("POST", "/api/generate", HttpStatusCode.OK, primingResponseJson);
    }

    private static void ConfigureQueuedGenerateResponses(LlmProviderQwenHarness harness, Queue<string> responseBodies)
    {
        harness.SetOllamaState(httpReady: true, ready: false, bootstrapFailed: false);
        harness.OllamaHandler.On(
            "POST",
            "/api/generate",
            _ =>
            {
                if (responseBodies.Count == 0)
                {
                    throw new InvalidOperationException("No queued /api/generate response remained.");
                }

                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(responseBodies.Dequeue(), Encoding.UTF8, "application/json")
                };
            });
    }

    private static JsonElement ParseJsonElement(string json)
    {
        using var document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }

    private static async Task WaitForHealthStatusAsync(LlmProviderQwenHarness harness, string expectedStatus)
    {
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            using var response = await harness.GetHealthAsync();
            var payload = await response.Content.ReadFromJsonAsync<ModuleHealthResponse>();
            if (payload is not null && string.Equals(payload.Status, expectedStatus, StringComparison.Ordinal))
            {
                return;
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"Qwen health did not reach status '{expectedStatus}' in time.");
    }
}
