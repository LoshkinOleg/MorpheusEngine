using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.Director;

[Collection("EngineProcessState")]
[Trait("Category", "Integration")]
public sealed class DirectorEndpointTests
{
    [Fact]
    public async Task Director_PostMessage_ValidInput_ReturnsDirectorMessageResponseWithText()
    {
        await using var harness = await DirectorHarness.CreateAsync();
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
                    new ChatGenerateResponse(true, "You stand still and listen.", """{"done":true}"""));
            });

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Text.Should().Be("You stand still and listen.");

        forwardedBody.Should().NotBeNull();
        using var document = JsonDocument.Parse(forwardedBody!);
        document.RootElement.GetProperty("sourceModule").GetString().Should().Be("director");
        document.RootElement.GetProperty("targetModule").GetString().Should().Be("generic_llm_provider");
        document.RootElement.GetProperty("targetPath").GetString().Should().Be("/chat");
        document.RootElement.GetProperty("method").GetString().Should().Be("POST");
        document.RootElement.GetProperty("body").GetProperty("messages")[1].GetProperty("content").GetString().Should().Be("look around");
    }

    [Fact]
    public async Task Director_PostMessage_BeforeBind_ReturnsBadRequest()
    {
        await using var harness = await DirectorHarness.CreateAsync();

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeFalse();
        payload.Error.Should().Contain("Director run is not bound");
    }

    [Fact]
    public async Task Director_PostMessage_EmptyPlayerInput_ReturnsBadRequest()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        using var response = await harness.PostMessageAsync(1, "   ");

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("Request must include non-empty playerInput.");
    }

    [Fact]
    public async Task Director_PostMessage_TurnLessThanOne_ReturnsBadRequest()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        using var response = await harness.PostMessageAsync(0, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("Turn must be >= 1.");
    }

    [Fact]
    public async Task Director_PostMessage_SuccessiveSuccessfulCalls_AppendUserAndAssistantToHistory()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        var forwardedBodies = new List<string>();
        var responses = new Queue<ChatGenerateResponse>(
        [
            new ChatGenerateResponse(true, "You stand still and listen.", """{"done":true}"""),
            new ChatGenerateResponse(true, "The door creaks open.", """{"done":true}""")
        ]);
        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                forwardedBodies.Add(request.Content!.ReadAsStringAsync().GetAwaiter().GetResult());
                return BuildJsonHttpResponse(HttpStatusCode.OK, responses.Dequeue());
            });

        using var firstResponse = await harness.PostMessageAsync(1, "look around");
        using var secondResponse = await harness.PostMessageAsync(2, "open the door");

        firstResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        secondResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        forwardedBodies.Should().HaveCount(2);

        var firstMessages = ReadMessages(forwardedBodies[0]);
        firstMessages.Should().HaveCount(2);
        firstMessages[0].Role.Should().Be("system");
        firstMessages[1].Role.Should().Be("user");
        firstMessages[1].Content.Should().Be("look around");

        var secondMessages = ReadMessages(forwardedBodies[1]);
        secondMessages.Should().HaveCount(4);
        secondMessages[1].Role.Should().Be("user");
        secondMessages[1].Content.Should().Be("look around");
        secondMessages[2].Role.Should().Be("assistant");
        secondMessages[2].Content.Should().Be("You stand still and listen.");
        secondMessages[3].Role.Should().Be("user");
        secondMessages[3].Content.Should().Be("open the door");
    }

    [Fact]
    public async Task Director_PostMessage_LlmFailure_DoesNotAppendOrphanUserRow()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        var forwardedBodies = new List<string>();
        var handlerCallCount = 0;
        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                forwardedBodies.Add(request.Content!.ReadAsStringAsync().GetAwaiter().GetResult());
                handlerCallCount++;
                return handlerCallCount == 1
                    ? BuildJsonHttpResponse(HttpStatusCode.OK, new ChatGenerateResponse(false, null, """{"done":true}"""))
                    : BuildJsonHttpResponse(HttpStatusCode.OK, new ChatGenerateResponse(true, "You stand still and listen.", """{"done":true}"""));
            });

        using var failedResponse = await harness.PostMessageAsync(1, "look around");
        using var successfulResponse = await harness.PostMessageAsync(1, "look around");

        failedResponse.StatusCode.Should().Be((HttpStatusCode)422);
        successfulResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        forwardedBodies.Should().HaveCount(2);

        var firstMessages = ReadMessages(forwardedBodies[0]);
        var secondMessages = ReadMessages(forwardedBodies[1]);
        firstMessages.Should().HaveCount(2);
        secondMessages.Should().HaveCount(2);
        secondMessages[0].Role.Should().Be("system");
        secondMessages[1].Role.Should().Be("user");
        secondMessages[1].Content.Should().Be("look around");
    }

    [Fact]
    public async Task Director_PostMessage_LlmReturnsOkFalse_ReturnsUnprocessableEntity()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            BuildJsonHandler(HttpStatusCode.OK, new ChatGenerateResponse(false, null, """{"done":true}""")));

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be((HttpStatusCode)422);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("LLM chat response was empty or missing 'response'.");
    }

    [Fact]
    public async Task Director_PostMessage_NonJsonLlmResponse_ReturnsUnprocessableEntity()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("not-json", System.Text.Encoding.UTF8, "application/json")
            });

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be((HttpStatusCode)422);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("Proxied LLM response was not valid JSON.");
    }

    [Fact]
    public async Task Director_PostMessage_RouterProxyUnreachable_ReturnsBadGateway()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            _ => throw new HttpRequestException("connection refused"));

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.BadGateway);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Error.Should().Be("Failed to reach router proxy for LLM chat.");
    }

    [Fact]
    public async Task Director_PostInitialize_LoadsSystemPromptAndLoreIntoHistory()
    {
        await using var harness = await DirectorHarness.CreateAsync();

        string? forwardedBody = null;
        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                forwardedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                return BuildJsonHttpResponse(
                    HttpStatusCode.OK,
                    new ChatGenerateResponse(true, "You stand still and listen.", """{"done":true}"""));
            });

        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        using var messageResponse = await harness.PostMessageAsync(1, "look around");

        messageResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        forwardedBody.Should().NotBeNull();

        var messages = ReadMessages(forwardedBody!);
        messages[0].Role.Should().Be("system");
        messages[0].Content.Should().Contain("You are the game master for a focused test scenario.");
        messages[0].Content.Should().Contain("## Canon Lore");
        messages[0].Content.Should().Contain("Ancient Ruins");
        messages[0].Content.Should().Contain("Oasis City");
    }

    [Fact]
    public async Task Director_ConcurrentPostMessageCalls_AreSerializedBySemaphoreSlim()
    {
        await using var harness = await DirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        var forwardedBodies = new List<string>();
        var firstRequestEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstRequest = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var callCount = 0;

        harness.ProxyHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                var currentCall = Interlocked.Increment(ref callCount);
                forwardedBodies.Add(request.Content!.ReadAsStringAsync().GetAwaiter().GetResult());
                if (currentCall == 1)
                {
                    firstRequestEntered.SetResult();
                    releaseFirstRequest.Task.GetAwaiter().GetResult();
                    return BuildJsonHttpResponse(
                        HttpStatusCode.OK,
                        new ChatGenerateResponse(true, "First response", """{"done":true}"""));
                }

                return BuildJsonHttpResponse(
                    HttpStatusCode.OK,
                    new ChatGenerateResponse(true, "Second response", """{"done":true}"""));
            });

        var firstTask = harness.PostMessageAsync(1, "look around");
        await firstRequestEntered.Task;

        var secondTask = harness.PostMessageAsync(2, "open the door");
        await Task.Delay(100);
        callCount.Should().Be(1);

        releaseFirstRequest.SetResult();

        using var firstResponse = await firstTask;
        using var secondResponse = await secondTask;

        firstResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        secondResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        callCount.Should().Be(2);
        forwardedBodies.Should().HaveCount(2);

        var secondMessages = ReadMessages(forwardedBodies[1]);
        secondMessages.Should().HaveCount(4);
        secondMessages[1].Content.Should().Be("look around");
        secondMessages[2].Role.Should().Be("assistant");
        secondMessages[2].Content.Should().Be("First response");
        secondMessages[3].Content.Should().Be("open the door");
    }

    private static IReadOnlyList<(string Role, string Content)> ReadMessages(string proxyBody)
    {
        using var document = JsonDocument.Parse(proxyBody);
        return document.RootElement
            .GetProperty("body")
            .GetProperty("messages")
            .EnumerateArray()
            .Select(message => (
                Role: message.GetProperty("role").GetString() ?? string.Empty,
                Content: message.GetProperty("content").GetString() ?? string.Empty))
            .ToArray();
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
