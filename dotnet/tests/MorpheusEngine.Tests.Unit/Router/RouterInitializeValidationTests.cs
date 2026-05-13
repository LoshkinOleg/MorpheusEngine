using FluentAssertions;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterInitializeValidationTests
{
    [Fact]
    // Verifies that initialize payload requires both routing identifiers.
    public void Router_ValidateInitializePayload_MissingIds_ReturnsError()
    {
        var error = RouterType.ValidateInitializePayload(new InitializeModuleRequest(
            "project-1",
            " "));

        error.Should().NotBeNull();
        error!.Error.Should().Contain("runId and gameProjectId");
    }

    [Fact]
    // Verifies that a well-formed initialize payload passes precondition validation.
    public void Router_ValidateInitializePayload_ValidPayload_ReturnsNull()
    {
        var error = RouterType.ValidateInitializePayload(new InitializeModuleRequest(
            "project-1",
            "run-1"));

        error.Should().BeNull();
    }

    [Fact]
    // Verifies that /turn preconditions fail when the router has not been bound yet.
    public void Router_ValidateTurnRequest_NotBound_ReturnsBindingError()
    {
        var error = RouterType.ValidateTurnRequest(new TurnRequest(1, "hello"), runBound: false);

        error.Should().NotBeNull();
        error!.Error.Should().Contain("not bound");
    }

    [Fact]
    // Verifies that turn index must stay positive for deterministic sequencing.
    public void Router_ValidateTurnRequest_InvalidTurn_ReturnsError()
    {
        var error = RouterType.ValidateTurnRequest(new TurnRequest(0, "hello"), runBound: true);

        error.Should().NotBeNull();
        error!.Error.Should().Contain("Turn must be >= 1");
    }

    [Fact]
    // Verifies that a bound router with valid turn payload passes validation.
    public void Router_ValidateTurnRequest_ValidPayload_ReturnsNull()
    {
        var error = RouterType.ValidateTurnRequest(new TurnRequest(3, "hello"), runBound: true);

        error.Should().BeNull();
    }
}
