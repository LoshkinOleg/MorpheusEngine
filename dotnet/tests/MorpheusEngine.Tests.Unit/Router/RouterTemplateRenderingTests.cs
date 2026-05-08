using FluentAssertions;
using RouterType = global::MorpheusEngine.Router;
using System.Text.Json;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterTemplateRenderingTests
{
    // Verifies that the turn placeholder renders as an integer literal.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_ReplacesTurnPlaceholderWithInteger()
    {
        var result = RouterType.RenderTurnPipelineStepBody(
            """{"turn":{{turn}}}""",
            7,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        result.Should().Be("""{"turn":7}""");
    }

    // Verifies that player input is JSON-escaped when rendered into a body.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_ReplacesPlayerInputJsonWithEscapedString()
    {
        var result = RouterType.RenderTurnPipelineStepBody(
            """{"playerInput":{{playerInputJson}}}""",
            1,
            "say \"hello\"\nand wave",
            previousResult: null,
            stepResults: NoStepResults());

        using var document = JsonDocument.Parse(result);
        document.RootElement.GetProperty("playerInput").GetString().Should().Be("say \"hello\"\nand wave");
    }

    // Verifies that previous.rawBody injects the prior step response as JSON.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_ReplacesPreviousRawBodyWithPriorStepBody()
    {
        var previous = StepResult("""{"ok":true,"text":"prior"}""");

        var result = RouterType.RenderTurnPipelineStepBody(
            """{"previous":{{previous.rawBody}}}""",
            1,
            "look around",
            previous,
            NoStepResults());

        result.Should().Be("""{"previous":{"ok":true,"text":"prior"}}""");
    }

    // Verifies that previous.rawBodyJson serializes the prior step body as a string.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_ReplacesPreviousRawBodyJsonWithSerializedPriorBody()
    {
        var previous = StepResult("""{"ok":true,"text":"prior"}""");

        var result = RouterType.RenderTurnPipelineStepBody(
            """{"previous":{{previous.rawBodyJson}}}""",
            1,
            "look around",
            previous,
            NoStepResults());

        using var document = JsonDocument.Parse(result);
        document.RootElement.GetProperty("previous").GetString().Should().Be("""{"ok":true,"text":"prior"}""");
    }

    // Verifies that named step placeholders resolve to earlier step output.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_ReplacesNamedStepRawBody()
    {
        var stepResults = new Dictionary<string, RouterType.ForwardedModuleResult>(StringComparer.OrdinalIgnoreCase)
        {
            ["director_message"] = StepResult("""{"ok":true,"text":"director"}""")
        };

        var result = RouterType.RenderTurnPipelineStepBody(
            """{"director":{{step.director_message.rawBody}}}""",
            1,
            "look around",
            previousResult: null,
            stepResults);

        result.Should().Be("""{"director":{"ok":true,"text":"director"}}""");
    }

    // Verifies that previous.rawBody fails before any step result exists.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_PreviousRawBodyBeforeAnyStep_ThrowsInvalidOperationException()
    {
        var act = () => RouterType.RenderTurnPipelineStepBody(
            """{"previous":{{previous.rawBody}}}""",
            1,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*previous.rawBody before any step executed*");
    }

    // Verifies that references to unknown steps fail during rendering.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_UnknownStepId_ThrowsInvalidOperationException()
    {
        var act = () => RouterType.RenderTurnPipelineStepBody(
            """{"director":{{step.missing_step.rawBody}}}""",
            1,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*referenced step 'missing_step' before it executed*");
    }

    // Verifies that unterminated placeholders are rejected.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_UnterminatedPlaceholder_ThrowsInvalidOperationException()
    {
        var act = () => RouterType.RenderTurnPipelineStepBody(
            """{"turn":{{turn}""",
            1,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*unterminated placeholder*");
    }

    // Verifies that unsupported placeholders are rejected.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_UnsupportedPlaceholder_ThrowsInvalidOperationException()
    {
        var act = () => RouterType.RenderTurnPipelineStepBody(
            """{"value":{{unsupported}}}""",
            1,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*Unsupported turn pipeline placeholder*");
    }

    // Verifies that invalid rendered JSON surfaces a JSON parse error.
    [Fact]
    public void Router_RenderTurnPipelineStepBody_InvalidRenderedJson_ThrowsJsonException()
    {
        var act = () => RouterType.RenderTurnPipelineStepBody(
            """{"turn":{{turn}}""",
            1,
            "look around",
            previousResult: null,
            stepResults: NoStepResults());

        act.Should().Throw<System.Text.Json.JsonException>();
    }

    private static Dictionary<string, RouterType.ForwardedModuleResult> NoStepResults()
    {
        return new Dictionary<string, RouterType.ForwardedModuleResult>(StringComparer.OrdinalIgnoreCase);
    }

    private static RouterType.ForwardedModuleResult StepResult(string body)
    {
        return new RouterType.ForwardedModuleResult(200, "application/json", body);
    }
}
