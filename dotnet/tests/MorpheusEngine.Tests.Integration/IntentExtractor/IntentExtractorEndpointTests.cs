using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Integration.IntentExtractor;

[Trait("Category", "Integration")]
public sealed class IntentExtractorEndpointTests
{
    // Verifies that valid input returns a structured intent and proxies the LLM request.
    [Fact]
    public async Task IntentExtractor_PostIntent_ValidPlayerInput_ReturnsStructuredIntentResponse()
    {
        await using var harness = await IntentExtractorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        string? forwardedBody = null;
        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                forwardedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                return BuildJsonHttpResponse(
                    HttpStatusCode.OK,
                    new LlmProviderGenerateResponse(
                        true,
                        """{"intent":"inspect","params":{"target":"airlock"}}""",
                        """{"done":true}"""));
            });

        using var response = await harness.PostIntentAsync("inspect the airlock");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<IntentResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Intent.Should().Be("inspect");
        payload.Parameters.Should().ContainKey("target").WhoseValue.Should().Be("airlock");

        forwardedBody.Should().NotBeNull();
        using var proxyDocument = JsonDocument.Parse(forwardedBody!);
        proxyDocument.RootElement.GetProperty("sourceModule").GetString().Should().Be("intent_extractor");
        proxyDocument.RootElement.GetProperty("targetModule").GetString().Should().Be("generic_llm_provider");
        proxyDocument.RootElement.GetProperty("targetPath").GetString().Should().Be("/generate");
        proxyDocument.RootElement.GetProperty("method").GetString().Should().Be("POST");
        proxyDocument.RootElement.GetProperty("body").GetProperty("prompt").GetString().Should().Contain("inspect the airlock");
    }

    // Verifies that the intent endpoint rejects requests before initialization.
    [Fact]
    public async Task IntentExtractor_PostIntent_BeforeBind_ReturnsError()
    {
        await using var harness = await IntentExtractorHarness.CreateAsync();
        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            BuildJsonHandler(
                HttpStatusCode.OK,
                new LlmProviderGenerateResponse(
                    true,
                    """{"intent":"wait","params":{}}""",
                    """{"done":true}""")));

        using var response = await harness.PostIntentAsync("wait");

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeFalse();
        payload.Error.Should().Contain("No bound run");
    }

    // Verifies that unparseable LLM output returns the fallback parse error.
    [Fact]
    public async Task IntentExtractor_PostIntent_UnparseableLlmResponse_ReturnsFallbackError()
    {
        await using var harness = await IntentExtractorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            BuildJsonHandler(
                HttpStatusCode.OK,
                new LlmProviderGenerateResponse(
                    true,
                    "not valid intent output",
                    """{"done":true}""")));

        using var response = await harness.PostIntentAsync("do something strange");

        response.StatusCode.Should().Be((HttpStatusCode)422);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeFalse();
        payload.Error.Should().Be("Could not parse LLM output as intent JSON.");
        payload.Details.Should().Contain("not valid intent output");
    }

    // Verifies that initialization binds the run and reports a healthy state.
    [Fact]
    public async Task IntentExtractor_PostInitialize_BindsRunSuccessfully()
    {
        await using var harness = await IntentExtractorHarness.CreateAsync();

        using var response = await harness.InitializeAsync();

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<InitializeModuleResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();

        using var healthResponse = await harness.Client.GetAsync("/health");
        healthResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        var healthPayload = await healthResponse.Content.ReadFromJsonAsync<ModuleHealthResponse>();
        healthPayload.Should().NotBeNull();
        healthPayload!.Ok.Should().BeTrue();
        healthPayload.Status.Should().Be("healthy");
        healthPayload.Initialized.Should().BeTrue();
    }

    private static Func<HttpRequestMessage, HttpResponseMessage> BuildJsonHandler<T>(HttpStatusCode statusCode, T payload)
    {
        return _ => BuildJsonHttpResponse(statusCode, payload);
    }

    private static HttpResponseMessage BuildJsonHttpResponse<T>(HttpStatusCode statusCode, T payload)
    {
        return new HttpResponseMessage(statusCode)
        {
            Content = JsonContent.Create(payload)
        };
    }
}
