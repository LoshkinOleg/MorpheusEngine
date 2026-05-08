using System.Text.Json;
using FluentAssertions;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;

namespace MorpheusEngine.Tests.Unit.MemoryDirector;

[Trait("Category", "Unit")]
public sealed class MemoryDirectorToolExecutionTests
{
    // Verifies that send_message returns a terminal message payload.
    [Fact]
    public void MemoryDirector_ExecuteSendMessage_ReturnsTerminalMessage()
    {
        var result = MemoryDirectorType.ExecuteSendMessage(ParseJson("""{"message":"You stand still and listen."}"""));

        result.Ok.Should().BeTrue();
        result.FinalMessage.Should().Be("You stand still and listen.");
        using var document = JsonDocument.Parse(result.ToolResultContent);
        document.RootElement.GetProperty("sent").GetBoolean().Should().BeTrue();
    }

    // Verifies that appending to a non-empty block inserts a newline separator.
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

    // Verifies that appending to an empty block sets the content directly.
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

    // Verifies that replacing a matching old value updates the block content.
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

    // Verifies that replace throws when the requested old value is missing.
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

    // Verifies that replace throws when the requested old value is ambiguous.
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

    // Verifies that set overwrites the entire memory block value.
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

    // Verifies that block updates reject writes to read-only blocks.
    [Fact]
    public void MemoryDirector_BuildBlockUpdateResult_ReadOnlyBlock_ThrowsInvalidOperationException()
    {
        var before = CreateMemoryBlock(label: "persona", value: "Keep tone mysterious.", readOnly: true);
        var after = before with { Value = "Keep tone ominous." };

        var act = () => MemoryDirectorType.BuildBlockUpdateResult(2, 1, "core_memory_set", before, after);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'persona' is read-only.");
    }

    // Verifies that block updates reject values that exceed the character limit.
    [Fact]
    public void MemoryDirector_BuildBlockUpdateResult_ValueExceedsCharLimit_ThrowsInvalidOperationException()
    {
        var before = CreateMemoryBlock(label: "player", value: "short", charLimit: 10);
        var after = before with { Value = "this value is too long" };

        var act = () => MemoryDirectorType.BuildBlockUpdateResult(2, 1, "core_memory_set", before, after);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("Memory block 'player' value length 22 exceeds charLimit 10.");
    }

    // Verifies that the current snapshot tool returns the latest snapshot payload.
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
