using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Integration.Embeddings;

[Collection("EngineProcessState")]
[Trait("Category", "Integration")]
public sealed class EmbeddingsOllamaIntegrationTests
{
    // Verifies that embedding valid text returns a vector payload with expected metadata.
    [Fact]
    public async Task EmbeddingsOllama_PostEmbed_ValidText_ReturnsVectorWithExpectedDimensions()
    {
        await using var harness = await EmbeddingsOllamaHarness.CreateAsync();
        harness.OllamaHandler.OnJson("GET", "/", HttpStatusCode.OK, """{"status":"ready"}""");
        await WaitForHealthStatusAsync(harness, "awaiting_initialize");

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        await WaitForHealthStatusAsync(harness, "healthy");

        harness.OllamaHandler.OnJson(
            "POST",
            "/api/embed",
            HttpStatusCode.OK,
            """{"embeddings":[[0.1,0.2,0.3,0.4]]}""");

        using var response = await harness.PostEmbedAsync(["inspect the ruins"]);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<EmbeddingResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Model.Should().Be("nomic-embed-text");
        payload.Dimensions.Should().Be(4);
        payload.Vectors.Should().ContainSingle();
        payload.Vectors[0].Index.Should().Be(0);
        payload.Vectors[0].Vector.Should().Equal(0.1f, 0.2f, 0.3f, 0.4f);

        var embedRequest = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/embed");
        using var requestDocument = JsonDocument.Parse(embedRequest.Body);
        requestDocument.RootElement.GetProperty("model").GetString().Should().Be("nomic-embed-text");
        requestDocument.RootElement.GetProperty("input")[0].GetString().Should().Be("inspect the ruins");
        requestDocument.RootElement.GetProperty("keep_alive").GetString().Should().Be("30m");
        requestDocument.RootElement.GetProperty("options").GetProperty("num_ctx").GetInt32().Should().Be(2048);
    }

    // Verifies that embedding multiple texts returns one vector per input.
    [Fact]
    public async Task EmbeddingsOllama_PostEmbed_MultipleTexts_ReturnsOneVectorPerText()
    {
        await using var harness = await EmbeddingsOllamaHarness.CreateAsync();
        harness.OllamaHandler.OnJson("GET", "/", HttpStatusCode.OK, """{"status":"ready"}""");
        await WaitForHealthStatusAsync(harness, "awaiting_initialize");

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        await WaitForHealthStatusAsync(harness, "healthy");

        harness.OllamaHandler.OnJson(
            "POST",
            "/api/embed",
            HttpStatusCode.OK,
            """{"embeddings":[[1.0,1.1],[2.0,2.1]]}""");

        using var response = await harness.PostEmbedAsync(["first clue", "second clue"]);

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<EmbeddingResponse>();
        payload.Should().NotBeNull();
        payload!.Vectors.Should().HaveCount(2);
        payload.Dimensions.Should().Be(2);
        payload.Vectors[0].Index.Should().Be(0);
        payload.Vectors[1].Index.Should().Be(1);
        payload.Vectors[0].Vector.Should().Equal(1.0f, 1.1f);
        payload.Vectors[1].Vector.Should().Equal(2.0f, 2.1f);

        var embedRequest = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/embed");
        using var requestDocument = JsonDocument.Parse(embedRequest.Body);
        requestDocument.RootElement.GetProperty("input").EnumerateArray().Select(item => item.GetString())
            .Should().Equal("first clue", "second clue");
    }

    // Verifies that token counting returns the model prompt evaluation count.
    [Fact]
    public async Task EmbeddingsOllama_PostTokenCount_ReturnsPromptEvalCount()
    {
        await using var harness = await EmbeddingsOllamaHarness.CreateAsync();
        harness.OllamaHandler.OnJson("GET", "/", HttpStatusCode.OK, """{"status":"ready"}""");
        await WaitForHealthStatusAsync(harness, "awaiting_initialize");

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        await WaitForHealthStatusAsync(harness, "healthy");

        harness.OllamaHandler.OnJson(
            "POST",
            "/api/generate",
            HttpStatusCode.OK,
            """{"prompt_eval_count":17,"done":true}""");

        using var response = await harness.PostTokenCountAsync("count these tokens");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<TokenCountResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Model.Should().Be("nomic-embed-text");
        payload.EstimatedTokens.Should().Be(17);
        payload.Exact.Should().BeTrue();

        var tokenRequest = harness.OllamaHandler.CapturedRequests.Last(request => request.Path == "/api/generate");
        using var requestDocument = JsonDocument.Parse(tokenRequest.Body);
        requestDocument.RootElement.GetProperty("model").GetString().Should().Be("nomic-embed-text");
        requestDocument.RootElement.GetProperty("prompt").GetString().Should().Be("count these tokens");
        requestDocument.RootElement.GetProperty("options").GetProperty("num_predict").GetInt32().Should().Be(0);
    }

    // Verifies that health transitions from startup to awaiting initialization when Ollama becomes ready.
    [Fact]
    public async Task EmbeddingsOllama_Health_ReportsStartupThenReadyStates()
    {
        await using var harness = await EmbeddingsOllamaHarness.CreateAsync();
        var readyGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        harness.OllamaHandler.OnAsync(
            "GET",
            "/",
            async (_, cancellationToken) =>
            {
                await readyGate.Task.WaitAsync(cancellationToken);
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = JsonContent.Create(new { status = "ready" })
                };
            });

        await WaitForHealthStatusAsync(harness, "ollama_starting");
        readyGate.SetResult();
        await WaitForHealthStatusAsync(harness, "awaiting_initialize");
    }

    // Verifies that embedding requests fail before the module is initialized.
    [Fact]
    public async Task EmbeddingsOllama_PostEmbed_BeforeBind_ReturnsConflict()
    {
        await using var harness = await EmbeddingsOllamaHarness.CreateAsync();

        using var response = await harness.PostEmbedAsync(["inspect the ruins"]);

        response.StatusCode.Should().Be(HttpStatusCode.Conflict);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("Module must be initialized before /embed.");
    }

    private static async Task WaitForHealthStatusAsync(EmbeddingsOllamaHarness harness, string expectedStatus)
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

        throw new TimeoutException($"Embeddings health did not reach status '{expectedStatus}' in time.");
    }
}
