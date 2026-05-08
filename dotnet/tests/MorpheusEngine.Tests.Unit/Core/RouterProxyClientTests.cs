using System.Net;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Helpers;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
public sealed class RouterProxyClientTests
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    [Fact]
    public async Task RouterProxyClient_PostAsync_SuccessfulJsonResponse_DeserializesPayload()
    {
        var handler = new MockHttpHandler();
        handler.On(
            "POST",
            "/proxy",
            _ => new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("""{"ok":true,"text":"You stand still and listen."}""")
            });
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var response = await client.PostAsync<TurnRequest, TurnResponse>(
            "generic_director",
            "turn",
            new TurnRequest(1, "look around"));

        response.StatusCode.Should().Be(200);
        response.DeserializeError.Should().BeNull();
        response.Payload.Should().Be(new TurnResponse(true, "You stand still and listen."));
        response.RawBody.Should().Contain("You stand still and listen.");
    }

    [Fact]
    public async Task RouterProxyClient_PostAsync_NonSuccessStatus_ReturnsStatusCodeAndRawBodyWithoutPayload()
    {
        var handler = new MockHttpHandler();
        handler.On("POST", "/proxy", HttpStatusCode.BadGateway, """{"ok":false,"error":"upstream failed"}""");
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var response = await client.PostAsync<TurnRequest, TurnResponse>(
            "generic_director",
            "/turn",
            new TurnRequest(1, "look around"));

        response.StatusCode.Should().Be(502);
        response.RawBody.Should().Be("""{"ok":false,"error":"upstream failed"}""");
        response.Payload.Should().BeNull();
        response.DeserializeError.Should().BeNull();
    }

    [Fact]
    public async Task RouterProxyClient_PostAsync_SuccessStatusWithUnparseableJson_ReturnsDeserializeError()
    {
        var handler = new MockHttpHandler();
        handler.On("POST", "/proxy", HttpStatusCode.OK, """{"ok":true,"text":}""");
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var response = await client.PostAsync<TurnRequest, TurnResponse>(
            "generic_director",
            "/turn",
            new TurnRequest(1, "look around"));

        response.StatusCode.Should().Be(200);
        response.Payload.Should().BeNull();
        response.DeserializeError.Should().NotBeNullOrWhiteSpace();
        response.RawBody.Should().Be("""{"ok":true,"text":}""");
    }

    [Fact]
    public async Task RouterProxyClient_PostAsync_EmptyTargetModule_ThrowsArgumentException()
    {
        var handler = new MockHttpHandler();
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var act = () => client.PostAsync<TurnRequest, TurnResponse>(
            string.Empty,
            "/turn",
            new TurnRequest(1, "look around"));

        var exception = await act.Should().ThrowAsync<ArgumentException>();
        exception.Which.ParamName.Should().Be("targetModule");
    }

    [Fact]
    public async Task RouterProxyClient_PostAsync_EmptyTargetPath_ThrowsArgumentException()
    {
        var handler = new MockHttpHandler();
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var act = () => client.PostAsync<TurnRequest, TurnResponse>(
            "generic_director",
            string.Empty,
            new TurnRequest(1, "look around"));

        var exception = await act.Should().ThrowAsync<ArgumentException>();
        exception.Which.ParamName.Should().Be("targetPath");
    }

    [Fact]
    public async Task RouterProxyClient_PostAsync_HttpRequestException_IsWrappedInInvalidOperationException()
    {
        var handler = new MockHttpHandler();
        handler.On("POST", "/proxy", _ => throw new HttpRequestException("connection refused"));
        using var httpClient = new HttpClient(handler);
        var client = CreateClient(httpClient);

        var act = () => client.PostAsync<TurnRequest, TurnResponse>(
            "generic_director",
            "/turn",
            new TurnRequest(1, "look around"));

        var exception = await act.Should().ThrowAsync<InvalidOperationException>();
        exception.Which.Message.Should().Contain("Router proxy request failed");
        exception.Which.InnerException.Should().BeOfType<HttpRequestException>();
    }

    private static RouterProxyClient CreateClient(HttpClient httpClient)
    {
        var configuration = new TestConfigBuilder()
            .AddModule("router", 19100, requiredByEngine: true)
            .Build();

        return new RouterProxyClient(httpClient, configuration, "memory_director", JsonOptions);
    }
}
