using FluentAssertions;
using MorpheusEngine.Tests.Unit.Helpers;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterProxyAllowlistTests
{
    [Fact]
    // Verifies that unknown modules are rejected with a deterministic 400 classification.
    public void Router_ResolveProxyTarget_UnknownModule_ReturnsBadRequestError()
    {
        var config = BuildConfiguration();

        var result = RouterType.ResolveProxyTarget(config, "missing_module", "/anything", "POST");

        result.Ok.Should().BeFalse();
        result.ErrorStatusCode.Should().Be(400);
        result.Error!.Error.Should().Contain("Unknown target module");
    }

    [Fact]
    // Verifies that module/path/method pairs not on the allowlist are rejected.
    public void Router_ResolveProxyTarget_ForbiddenPathOrMethod_ReturnsForbiddenError()
    {
        var config = BuildConfiguration();

        var result = RouterType.ResolveProxyTarget(config, "memory_director", "/shutdown", "POST");

        result.Ok.Should().BeFalse();
        result.ErrorStatusCode.Should().Be(403);
        result.Error!.Error.Should().Contain("not allowed by configuration");
    }

    [Fact]
    // Verifies successful module resolution and endpoint normalization.
    public void Router_ResolveProxyTarget_AllowedEndpoint_ReturnsResolvedTarget()
    {
        var config = BuildConfiguration();

        var result = RouterType.ResolveProxyTarget(config, "memory_director", "message", "post");

        result.Ok.Should().BeTrue();
        result.NormalizedPath.Should().Be("/message");
        result.MethodUpper.Should().Be("POST");
        result.TargetPort.Should().Be(7202);
        result.TargetModule.Should().NotBeNull();
        result.TargetModule!.PortKey.Should().Be("memory_director");
    }

    private static EngineConfiguration BuildConfiguration()
    {
        return new TestConfigBuilder()
            .AddModule(
                "router",
                7100,
                requiredByEngine: true,
                endpoints:
                [
                    new EngineEndpointInfo("/proxy", "Proxy", "POST", "module_proxy", null, null)
                ])
            .AddModule(
                "memory_director",
                7202,
                requiredByEngine: true,
                endpoints:
                [
                    new EngineEndpointInfo("/message", "Message", "POST", "director_message", null, null),
                    new EngineEndpointInfo("/health", "Health", "GET", "module_health", null, null)
                ])
            .Build();
    }
}
