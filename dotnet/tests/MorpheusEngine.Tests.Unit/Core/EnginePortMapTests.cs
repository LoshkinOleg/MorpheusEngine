using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
public sealed class EnginePortMapTests
{
    [Fact]
    // Verifies that a known module key returns its configured listen port.
    public void EnginePortMap_GetRequiredPort_KnownKey_ReturnsConfiguredPort()
    {
        var portMap = CreatePortMap();

        portMap.GetRequiredPort("router").Should().Be(19100);
    }

    [Fact]
    // Verifies that requesting an unknown module key throws a configuration error.
    public void EnginePortMap_GetRequiredPort_UnknownKey_ThrowsEngineConfigurationException()
    {
        var portMap = CreatePortMap();
        var act = () => portMap.GetRequiredPort("missing_module");

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*Unknown port_key 'missing_module'*");
    }

    [Fact]
    // Verifies that listen-port presence checks return the expected boolean for known and unknown modules.
    public void EnginePortMap_HasListenPortForModule_ReturnsExpectedBoolean()
    {
        var portMap = CreatePortMap();

        portMap.HasListenPortForModule("router").Should().BeTrue();
        portMap.HasListenPortForModule("missing_module").Should().BeFalse();
    }

    private static EnginePortMap CreatePortMap()
    {
        return new EnginePortMap(new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["router"] = 19100,
            ["memory_director"] = 19101
        });
    }
}
