using FluentAssertions;
using System.Text.Json;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterProxyValidationTests
{
    [Fact]
    // Verifies that malformed JSON returns the same invalid-payload error envelope as /proxy.
    public void Router_ValidateProxyRequestPayload_InvalidJson_ReturnsError()
    {
        var result = RouterType.ValidateProxyRequestPayload("{ invalid", new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });

        result.Ok.Should().BeFalse();
        result.Error.Should().NotBeNull();
        result.Error!.Error.Should().Contain("Invalid proxy request payload");
    }

    [Fact]
    // Verifies that required routing fields are enforced.
    public void Router_ValidateProxyRequestPayload_MissingFields_ReturnsError()
    {
        var result = RouterType.ValidateProxyRequestPayload(
            """{"sourceModule":"intent_extractor","method":"POST"}""",
            new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            });

        result.Ok.Should().BeFalse();
        result.Error!.Error.Should().Contain("sourceModule, targetModule, and targetPath");
    }

    [Fact]
    // Verifies that method is required and normalized to GET/POST only.
    public void Router_ValidateProxyRequestPayload_UnsupportedMethod_ReturnsError()
    {
        var result = RouterType.ValidateProxyRequestPayload(
            """
            {
              "sourceModule":"intent_extractor",
              "targetModule":"memory_director",
              "targetPath":"/message",
              "method":"DELETE"
            }
            """,
            new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            });

        result.Ok.Should().BeFalse();
        result.Error!.Error.Should().Contain("Unsupported proxy method");
    }

    [Fact]
    // Verifies that a valid payload is normalized and carried through.
    public void Router_ValidateProxyRequestPayload_ValidRequest_ReturnsNormalizedResult()
    {
        var result = RouterType.ValidateProxyRequestPayload(
            """
            {
              "sourceModule":" intent_extractor ",
              "targetModule":" memory_director ",
              "targetPath":"message",
              "method":"post",
              "body":{"turn":1}
            }
            """,
            new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            });

        result.Ok.Should().BeTrue();
        result.SourceModule.Should().Be("intent_extractor");
        result.TargetModule.Should().Be("memory_director");
        result.TargetPath.Should().Be("message");
        result.Method.Should().Be("POST");
        result.RequestBody.Should().Contain("\"turn\":1");
    }

    [Fact]
    // Verifies that GET forwarding does not attach an outbound request body.
    public void Router_BuildProxyOutboundRequest_Get_DoesNotSetContent()
    {
        using var request = RouterType.BuildProxyOutboundRequest("GET", "http://127.0.0.1:7202/health", requestBody: null);

        request.Method.Method.Should().Be("GET");
        request.Content.Should().BeNull();
    }

    [Fact]
    // Verifies that POST forwarding preserves JSON body and media type.
    public async Task Router_BuildProxyOutboundRequest_Post_SetsJsonContent()
    {
        using var request = RouterType.BuildProxyOutboundRequest("POST", "http://127.0.0.1:7202/message", """{"turn":1}""");

        request.Method.Method.Should().Be("POST");
        request.Content.Should().NotBeNull();
        request.Content!.Headers.ContentType!.MediaType.Should().Be("application/json");
        (await request.Content.ReadAsStringAsync()).Should().Be("""{"turn":1}""");
    }
}
