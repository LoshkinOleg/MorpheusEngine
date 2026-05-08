using System.Net;
using System.Net.Http.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Integration.MemoryDirector;

[Trait("Category", "Integration")]
public sealed class MemoryDirectorAgentLoopTests
{
    [Fact]
    public async Task MemoryDirector_PostMessage_FirstStepSendMessage_ReturnsPlayerFacingText()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.EnqueueChatAction(
            "Narrate the immediate result.",
            "send_message",
            """{"message":"You ease the vault door open."}""");

        using var response = await harness.PostMessageAsync(1, "open the vault");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Text.Should().Be("You ease the vault door open.");

        harness.ProxyHandler.ChatRequests.Should().HaveCount(1);
        harness.ProxyHandler.PersistStepRequests.Should().HaveCount(2);
        harness.ProxyHandler.Messages.Should().ContainSingle(message =>
            message.Turn == 1
            && message.StepNumber == 1
            && message.Role == "assistant"
            && message.MessageType == "send_message"
            && message.ToolName == "send_message"
            && message.Content.Contains("\"tool\":\"send_message\"", StringComparison.Ordinal));
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_CoreMemoryAppendThenSendMessage_UsesUpdatedBlockOnNextStep()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.EnqueueChatAction(
            "Record the new player fact before narrating.",
            "core_memory_append",
            """{"label":"player","content":"Carries a humming brass key."}""");
        harness.ProxyHandler.EnqueueChatAction(
            "Now narrate using the updated memory.",
            "send_message",
            """{"message":"The brass key vibrates in your palm as the vault responds."}""");

        using var response = await harness.PostMessageAsync(2, "hold up the brass key");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Text.Should().Be("The brass key vibrates in your palm as the vault responds.");

        harness.ProxyHandler.ChatRequests.Should().HaveCount(2);
        harness.ProxyHandler.Blocks.Should().ContainSingle(block =>
            block.Label == "player"
            && block.Value.Contains("Carries a humming brass key.", StringComparison.Ordinal));
        harness.ProxyHandler.Mutations.Should().ContainSingle(mutation =>
            mutation.ToolName == "core_memory_append"
            && mutation.Target == "player");
        harness.ProxyHandler.ChatRequests[1].Messages[0].Content.Should().Contain("Carries a humming brass key.");
        harness.ProxyHandler.Messages.Should().Contain(message =>
            message.Turn == 2
            && message.StepNumber == 1
            && message.Role == "tool"
            && message.MessageType == "tool_result"
            && message.ToolName == "core_memory_append");
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_WhenMaxStepsExceeded_ReturnsSynthesizedFallback()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync(maxStepsPerTurn: 2);
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.EnqueueChatAction(
            "I need one more inspection pass.",
            "get_current_snapshot",
            "{}");
        harness.ProxyHandler.EnqueueChatAction(
            "Still not ready; continue inspecting.",
            "get_current_snapshot",
            "{}");

        using var response = await harness.PostMessageAsync(3, "wait and observe");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Text.Should().Be("The scene settles for a moment. Still not ready; continue inspecting.");

        harness.ProxyHandler.PersistStepRequests.Should().HaveCount(4);
        harness.ProxyHandler.Messages.Should().Contain(message =>
            message.Turn == 3
            && message.StepNumber == 3
            && message.Role == "assistant"
            && message.MessageType == "send_message"
            && message.Content == "The scene settles for a moment. Still not ready; continue inspecting.");
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_UnknownTool_PersistsErrorAndContinuesLoop()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.EnqueueChatAction(
            "Try a tool that does not exist.",
            "open_portal",
            "{}");
        harness.ProxyHandler.EnqueueChatAction(
            "Recover and narrate plainly.",
            "send_message",
            """{"message":"You hesitate, then simply study the sealed archway."}""");

        using var response = await harness.PostMessageAsync(1, "use the portal controls");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Text.Should().Be("You hesitate, then simply study the sealed archway.");

        harness.ProxyHandler.Messages.Should().Contain(message =>
            message.Turn == 1
            && message.StepNumber == 1
            && message.Role == "tool"
            && message.MessageType == "tool_error"
            && message.Content.Contains("Unknown tool: open_portal", StringComparison.Ordinal));
        harness.ProxyHandler.ChatRequests.Should().HaveCount(2);
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_UnparseableJson_PersistsSchemaViolationAndRecovers()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        harness.ProxyHandler.EnqueueChatResponse(new ChatGenerateResponse(true, "not-json", """{"done":true}"""));
        harness.ProxyHandler.EnqueueChatAction(
            "Recover with a direct answer.",
            "send_message",
            """{"message":"A dry wind crosses the chamber, but nothing else changes."}""");

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<DirectorMessageResponse>();
        payload.Should().NotBeNull();
        payload!.Text.Should().Be("A dry wind crosses the chamber, but nothing else changes.");

        harness.ProxyHandler.Messages.Should().Contain(message =>
            message.Turn == 1
            && message.StepNumber == 1
            && message.Role == "tool"
            && message.MessageType == "tool_error"
            && message.ToolName == "schema_violation");
        harness.ProxyHandler.ChatRequests.Should().HaveCount(2);
    }

    [Fact]
    public async Task MemoryDirector_PostInitialize_LoadsAgentPromptAndSeedsCoreMemoryBlocks()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();

        using var initializeResponse = await harness.InitializeAsync();

        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);
        harness.ProxyHandler.Blocks.Select(block => block.Label).Should().Contain([
            "persona",
            "campaign_rules",
            "player",
            "current_scene",
            "objectives",
            "style",
            "world_summary"
        ]);
        harness.ProxyHandler.Blocks.Should().ContainSingle(block =>
            block.Label == "campaign_rules"
            && block.Value.Contains("focused test scenario", StringComparison.Ordinal));

        harness.ProxyHandler.EnqueueChatAction(
            "Prove the initialized prompt is in context.",
            "send_message",
            """{"message":"You steady your breath and study the room."}""");

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        harness.ProxyHandler.ChatRequests.Should().ContainSingle();
        harness.ProxyHandler.ChatRequests[0].Messages[0].Content.Should().Contain("You are the memory-managed test game master.");
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_BeforeBind_ReturnsBadRequest()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync();

        using var response = await harness.PostMessageAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.BadRequest);
        var payload = await response.Content.ReadFromJsonAsync<ErrorResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeFalse();
        payload.Error.Should().Contain("MemoryDirector run is not bound");
    }

    [Fact]
    public async Task MemoryDirector_PostMessage_WhenHistoryExceedsThreshold_TriggersRecallCompaction()
    {
        await using var harness = await MemoryDirectorHarness.CreateAsync(maxFullMessages: 2);
        using var initializeResponse = await harness.InitializeAsync();
        initializeResponse.StatusCode.Should().Be(HttpStatusCode.OK);

        SeedHistoricalMessages(harness.ProxyHandler);
        harness.ProxyHandler.EnqueueChatAction(
            "End the turn quickly.",
            "send_message",
            """{"message":"The old patrol routes click into focus."}""");

        using var response = await harness.PostMessageAsync(4, "review the patrol notes");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        harness.ProxyHandler.CompactionRequests.Should().ContainSingle();
        harness.ProxyHandler.Summaries.Should().ContainSingle();

        var compaction = harness.ProxyHandler.CompactionRequests[0];
        compaction.SourceMessageCount.Should().BeGreaterThan(0);
        compaction.EndTurn.Should().BeLessThan(4);
        compaction.Summary.Should().Contain("Summary of turns");
        harness.ProxyHandler.Messages.Should().NotContain(message => message.Turn == compaction.StartTurn);
    }

    private static void SeedHistoricalMessages(MemoryDirectorHarness.MockMemoryDirectorProxyHandler proxyHandler)
    {
        proxyHandler.Messages.AddRange(
        [
            CreateAgentMessage(1, 0, "player", "We mapped the outer wall.", "player_input"),
            CreateAgentMessage(1, 1, "assistant", "The sentry tower stays dark."),
            CreateAgentMessage(2, 0, "player", "Check the eastern gate.", "player_input"),
            CreateAgentMessage(2, 1, "assistant", "Tracks cluster near the hinge."),
            CreateAgentMessage(3, 0, "player", "Review the patrol notes.", "player_input"),
            CreateAgentMessage(3, 1, "assistant", "The route loops back before dawn.")
        ]);
    }

    private static AgentMessageDto CreateAgentMessage(
        int turn,
        int stepNumber,
        string role,
        string content,
        string messageType = "send_message",
        string? toolName = null)
    {
        return new AgentMessageDto(turn, stepNumber, role, messageType, content, toolName);
    }
}
