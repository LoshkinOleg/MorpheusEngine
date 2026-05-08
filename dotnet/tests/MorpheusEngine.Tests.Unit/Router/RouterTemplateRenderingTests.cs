using FluentAssertions;
using RouterType = global::MorpheusEngine.Router;
using System.Text.Json;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterTemplateRenderingTests
{
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
