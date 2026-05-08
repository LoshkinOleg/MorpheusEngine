using System.Text.Json;
using FluentAssertions;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;

namespace MorpheusEngine.Tests.Unit.MemoryDirector;

[Trait("Category", "Unit")]
public sealed class MemoryDirectorToolExecutionTests
{
    [Fact]
    public void MemoryDirector_ExecuteSendMessage_ReturnsTerminalMessage()
    {
        var result = MemoryDirectorType.ExecuteSendMessage(ParseJson("""{"message":"You stand still and listen."}"""));

        result.Ok.Should().BeTrue();
        result.FinalMessage.Should().Be("You stand still and listen.");
        using var document = JsonDocument.Parse(result.ToolResultContent);
        document.RootElement.GetProperty("sent").GetBoolean().Should().BeTrue();
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemoryAppend_NonEmptyBlock_AppendsWithNewlineSeparator()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "player", value: "Knows the access code.")
        ]);

        var result = MemoryDirectorType.ExecuteCoreMemoryAppend(
            turn: 3,
            step: 2,
            memoryContext,
            ParseJson("""{"label":"player","content":"Carries a lantern."}"""));

        result.Ok.Should().BeTrue();
        result.BlockUpdates.Should().ContainSingle();
        result.BlockUpdates[0].Value.Should().Be("Knows the access code." + Environment.NewLine + "Carries a lantern.");
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemoryAppend_EmptyBlock_SetsContentDirectly()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "player", value: string.Empty)
        ]);

        var result = MemoryDirectorType.ExecuteCoreMemoryAppend(
            turn: 3,
            step: 2,
            memoryContext,
            ParseJson("""{"label":"player","content":"Carries a lantern."}"""));

        result.Ok.Should().BeTrue();
        result.BlockUpdates.Should().ContainSingle();
        result.BlockUpdates[0].Value.Should().Be("Carries a lantern.");
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemoryReplace_ReplacesOldValueWithNewValue()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "current_scene", value: "The bronze door is sealed.")
        ]);

        var result = MemoryDirectorType.ExecuteCoreMemoryReplace(
            turn: 4,
            step: 1,
            memoryContext,
            ParseJson("""{"label":"current_scene","oldValue":"sealed","newValue":"open"}"""));

        result.Ok.Should().BeTrue();
        result.BlockUpdates.Should().ContainSingle();
        result.BlockUpdates[0].Value.Should().Be("The bronze door is open.");
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemoryReplace_MissingOldValue_ThrowsInvalidOperationException()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "current_scene", value: "The bronze door is sealed.")
        ]);

        var act = () => MemoryDirectorType.ExecuteCoreMemoryReplace(
            turn: 4,
            step: 1,
            memoryContext,
            ParseJson("""{"label":"current_scene","oldValue":"broken","newValue":"open"}"""));

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'current_scene' does not contain the requested oldValue.");
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemoryReplace_DuplicateOldValue_ThrowsInvalidOperationException()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "current_scene", value: "door door")
        ]);

        var act = () => MemoryDirectorType.ExecuteCoreMemoryReplace(
            turn: 4,
            step: 1,
            memoryContext,
            ParseJson("""{"label":"current_scene","oldValue":"door","newValue":"gate"}"""));

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'current_scene' contains oldValue more than once; replacement is ambiguous.");
    }

    [Fact]
    public void MemoryDirector_ExecuteCoreMemorySet_OverwritesEntireBlockValue()
    {
        var memoryContext = CreateMemoryContext([
            CreateMemoryBlock(label: "objectives", value: "Reach the bunker.")
        ]);

        var result = MemoryDirectorType.ExecuteCoreMemorySet(
            turn: 5,
            step: 1,
            memoryContext,
            ParseJson("""{"label":"objectives","value":"Escape before sunrise."}"""));

        result.Ok.Should().BeTrue();
        result.BlockUpdates.Should().ContainSingle();
        result.BlockUpdates[0].Value.Should().Be("Escape before sunrise.");
    }

    [Fact]
    public void MemoryDirector_BuildBlockUpdateResult_ReadOnlyBlock_ThrowsInvalidOperationException()
    {
        var before = CreateMemoryBlock(label: "persona", value: "Keep tone mysterious.", readOnly: true);
        var after = before with { Value = "Keep tone ominous." };

        var act = () => MemoryDirectorType.BuildBlockUpdateResult(2, 1, "core_memory_set", before, after);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'persona' is read-only.");
    }

    [Fact]
    public void MemoryDirector_BuildBlockUpdateResult_ValueExceedsCharLimit_ThrowsInvalidOperationException()
    {
        var before = CreateMemoryBlock(label: "player", value: "short", charLimit: 10);
        var after = before with { Value = "this value is too long" };

        var act = () => MemoryDirectorType.BuildBlockUpdateResult(2, 1, "core_memory_set", before, after);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'player' value length 22 exceeds charLimit 10.");
    }

    [Fact]
    public void MemoryDirector_ExecuteGetCurrentSnapshot_ReturnsSerializedLatestSnapshot()
    {
        var memoryContext = CreateMemoryContext(
            [],
            latestSnapshot: new LatestSnapshotDto(
                7,
                """{"location":"airlock"}""",
                """{"narration":"The corridor hums softly."}"""));

        var result = MemoryDirectorType.ExecuteGetCurrentSnapshot(memoryContext);

        result.Ok.Should().BeTrue();
        using var document = JsonDocument.Parse(result.ToolResultContent);
        document.RootElement.GetProperty("turn").GetInt32().Should().Be(7);
        document.RootElement.GetProperty("worldStateJson").GetString().Should().Be("""{"location":"airlock"}""");
        document.RootElement.GetProperty("viewStateJson").GetString().Should().Be("""{"narration":"The corridor hums softly."}""");
    }

    private static JsonElement ParseJson(string json)
    {
        using var document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }

    private static MemoryLoadContextResponse CreateMemoryContext(
        IReadOnlyList<MemoryBlockDto> blocks,
        LatestSnapshotDto? latestSnapshot = null)
    {
        return new MemoryLoadContextResponse(
            true,
            blocks,
            [],
            latestSnapshot ?? new LatestSnapshotDto(
                1,
                """{"gameProjectId":"test_game"}""",
                """{"directorResponse":{"ok":true}}"""),
            new MemoryBudgetDto(4096, 2867, 12, 4000));
    }

    private static MemoryBlockDto CreateMemoryBlock(
        string label,
        string description = "Stable player-facing facts.",
        string value = "Player prefers concise descriptions.",
        int charLimit = 2000,
        bool readOnly = false)
    {
        return new MemoryBlockDto(label, description, value, charLimit, readOnly);
    }
}
