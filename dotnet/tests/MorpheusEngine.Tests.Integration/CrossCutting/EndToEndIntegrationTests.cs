using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.CrossCutting;

[Trait("Category", "Integration")]
[Collection("EngineProcessState")]
public sealed class EndToEndIntegrationTests
{
    [Fact]
    public async Task RouterTurn_FullPipeline_PersistsExpectedTurnArtifacts()
    {
        await using var harness = await EndToEndHarness.CreateAsync();
        harness.QwenOllama!.EnqueueChatAction(
            "Narrate the scene outcome directly.",
            "send_message",
            """{"message":"A cold draft slips through the ruin."}""");

        using var response = await harness.PostTurnAsync(1, "look around");

        response.StatusCode.Should().Be(HttpStatusCode.OK);
        var payload = await response.Content.ReadFromJsonAsync<TurnResponse>();
        payload.Should().NotBeNull();
        payload!.Ok.Should().BeTrue();
        payload.Text.Should().Be("A cold draft slips through the ruin.");

        using var connection = harness.OpenConnection();
        var events = RunDbInspector.ReadEvents(connection);
        var agentMessages = RunDbInspector.ReadAgentMessages(connection);
        var pipelineEvents = RunDbInspector.ReadPipelineEvents(connection);
        var snapshot = RunDbInspector.ReadSnapshotForTurn(connection, turn: 1);

        RunDbInspector.CountRows(connection, "snapshots", "turn = 1").Should().Be(1);
        events.Should().HaveCount(2);
        events[0].EventType.Should().Be("player_input");
        events[1].EventType.Should().Be("module_trace");
        agentMessages.Should().Contain(message =>
            message.Turn == 1
            && message.Role == "assistant"
            && message.MessageType == "send_message");
        pipelineEvents.Should().NotBeEmpty();
        snapshot.WorldState.Should().Contain("\"gameProjectId\":\"test_game\"");

        using var tracePayload = JsonDocument.Parse(events[1].Payload);
        tracePayload.RootElement.GetProperty("narrationText").GetString().Should().Be("A cold draft slips through the ruin.");
        tracePayload.RootElement.GetProperty("playerInputEcho").GetString().Should().Be("look around");
    }

    [Fact]
    public async Task MemoryDirector_CoreMemoryEdit_PersistsAndAppearsInNextTurnContext()
    {
        await using var harness = await EndToEndHarness.CreateAsync();
        harness.QwenOllama!.EnqueueChatAction(
            "Persist the new player fact before narrating.",
            "core_memory_append",
            """{"label":"player","content":"Carries a humming brass key."}""");
        harness.QwenOllama.EnqueueChatAction(
            "Now narrate the visible result.",
            "send_message",
            """{"message":"The brass key hums softly in your hand."}""");
        harness.QwenOllama.EnqueueChatAction(
            "Use the remembered detail naturally.",
            "send_message",
            """{"message":"The brass key answers the lock with a low vibration."}""");

        using var firstTurn = await harness.PostTurnAsync(1, "lift the brass key");
        using var secondTurn = await harness.PostTurnAsync(2, "hold the key to the lock");

        firstTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        secondTurn.StatusCode.Should().Be(HttpStatusCode.OK);

        var blocks = await harness.GetMemoryBlocksAsync();
        blocks.Blocks.Should().Contain(block =>
            block.Label == "player"
            && block.Value.Contains("Carries a humming brass key.", StringComparison.Ordinal));

        var capturedRequests = harness.QwenOllama.GetCapturedChatRequests();
        capturedRequests.Should().HaveCount(3);
        CombineContents(capturedRequests[2].Messages).Should().Contain("Carries a humming brass key.");
    }

    [Fact]
    public async Task ArchivalInsert_ThenSearch_FindsInsertedPassageThroughRealEmbeddingFlow()
    {
        await using var harness = await EndToEndHarness.CreateAsync();
        harness.QwenOllama!.EnqueueChatAction(
            "Archive the stable fact before speaking.",
            "archival_memory_insert",
            """{"scope":"project","source":"player_discovery","tags":["vault","key"],"content":"The moon gate opens with a humming brass key."}""");
        harness.QwenOllama.EnqueueChatAction(
            "Confirm the archival write to the player.",
            "send_message",
            """{"message":"You commit the moon gate clue to long-term memory."}""");
        harness.QwenOllama.EnqueueChatAction(
            "Search the archival store for the clue.",
            "archival_memory_search",
            """{"query":"moon gate brass key","topK":3}""");
        harness.QwenOllama.EnqueueChatAction(
            "Narrate after reviewing the retrieved archival result.",
            "send_message",
            """{"message":"The archived clue confirms the brass key opens the moon gate."}""");

        using var firstTurn = await harness.PostTurnAsync(1, "memorize the moon gate clue");
        using var secondTurn = await harness.PostTurnAsync(2, "what opens the moon gate?");

        firstTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        secondTurn.StatusCode.Should().Be(HttpStatusCode.OK);

        using var connection = harness.OpenConnection();
        var archivalPassages = RunDbInspector.ReadArchivalPassages(connection);
        archivalPassages.Should().Contain(row =>
            row.Source == "player_discovery"
            && row.Content.Contains("moon gate opens with a humming brass key", StringComparison.OrdinalIgnoreCase));

        var capturedRequests = harness.QwenOllama.GetCapturedChatRequests();
        capturedRequests.Should().HaveCount(4);
        CombineContents(capturedRequests[3].Messages).Should().Contain("moon gate opens with a humming brass key");
    }

    [Fact]
    public async Task RecallCompaction_ReplacesOlderMessagesAndSummaryAppearsInLaterContext()
    {
        await using var harness = await EndToEndHarness.CreateAsync(maxFullMessages: 2);
        harness.QwenOllama!.EnqueueChatAction("Resolve turn one.", "send_message", """{"message":"Turn one settles into memory."}""");
        harness.QwenOllama.EnqueueChatAction("Resolve turn two.", "send_message", """{"message":"Turn two adds a fresh detail."}""");
        harness.QwenOllama.EnqueueChatAction("Resolve turn three.", "send_message", """{"message":"Turn three shifts the patrol route."}""");
        harness.QwenOllama.EnqueueChatAction("Resolve turn four.", "send_message", """{"message":"Turn four closes the scouting loop."}""");
        harness.QwenOllama.EnqueueChatAction("Resolve turn five with the summary in context.", "send_message", """{"message":"Turn five acts on the summarized patrol notes."}""");

        for (var turn = 1; turn <= 5; turn++)
        {
            using var response = await harness.PostTurnAsync(turn, $"take action {turn}");
            response.StatusCode.Should().Be(HttpStatusCode.OK);
        }

        using var connection = harness.OpenConnection();
        var summaries = RunDbInspector.ReadConversationSummaries(connection);
        var agentMessages = RunDbInspector.ReadAgentMessages(connection);

        summaries.Should().NotBeEmpty();
        summaries[0].Summary.Should().Contain("Summary of turns");
        agentMessages.Count.Should().BeLessThan(10);

        var capturedRequests = harness.QwenOllama.GetCapturedChatRequests();
        CombineContents(capturedRequests[^1].Messages).Should().Contain("Summary of turns");
    }

    [Fact]
    public async Task DefaultLoreCsv_IsSeededIntoArchivalStore_AndSearchReturnsRelevantEntry()
    {
        await using var harness = await EndToEndHarness.CreateAsync();

        using var connection = harness.OpenConnection();
        var loreRows = RunDbInspector.ReadLoreRows(connection);
        var archivalPassages = RunDbInspector.ReadArchivalPassages(connection);

        loreRows.Should().Contain(row =>
            row.Subject == "Ancient Ruins"
            && row.Data.Contains("northern desert", StringComparison.OrdinalIgnoreCase));
        archivalPassages.Should().Contain(row =>
            row.Source == "lore/default_lore_entries.csv"
            && row.Content.Contains("Ancient Ruins", StringComparison.Ordinal));

        var searchResponse = await harness.SearchArchivalAsync("northern desert ruins", topK: 3);

        searchResponse.Ok.Should().BeTrue();
        searchResponse.Results.Should().NotBeEmpty();
        searchResponse.Results[0].Content.Should().Contain("Ancient Ruins");
    }

    [Fact]
    public async Task GenericLlmAliasSwap_ReroutesTurnTrafficToConfiguredConcreteModule()
    {
        await using var qwenHarness = await EndToEndHarness.CreateAsync();
        qwenHarness.QwenOllama!.EnqueueChatAction(
            "Answer through the default qwen-backed provider.",
            "send_message",
            """{"message":"The qwen-backed route handled this turn."}""");

        using var qwenTurn = await qwenHarness.PostTurnAsync(1, "test the default llm alias");
        var qwenPayload = await qwenTurn.Content.ReadFromJsonAsync<TurnResponse>();

        qwenTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        qwenPayload!.Text.Should().Be("The qwen-backed route handled this turn.");
        qwenHarness.QwenOllama.CapturedChatRequestCount.Should().Be(1);

        await using var alternateHarness = await EndToEndHarness.CreateAsync(useAlternateLlmProvider: true);
        alternateHarness.AlternateProvider!.EnqueueChatAction(
            "Answer through the alternate provider.",
            "send_message",
            """{"message":"The alternate provider handled this turn."}""");

        using var alternateTurn = await alternateHarness.PostTurnAsync(1, "test the swapped llm alias");
        var alternatePayload = await alternateTurn.Content.ReadFromJsonAsync<TurnResponse>();

        alternateTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        alternatePayload!.Text.Should().Be("The alternate provider handled this turn.");
        alternateHarness.AlternateProvider.ChatRequests.Should().ContainSingle();
    }

    [Fact]
    public async Task ConcurrentTurns_DoNotReachTheLlmStageUntilTheEarlierTurnFinishes()
    {
        await using var harness = await EndToEndHarness.CreateAsync();
        var firstRequestStarted = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstTurn = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);

        harness.QwenOllama!.EnqueueBlockingChatAction(
            firstRequestStarted,
            releaseFirstTurn.Task,
            "Hold the first turn open until the test releases it.",
            "send_message",
            """{"message":"The first turn resolves cleanly."}""");
        harness.QwenOllama.EnqueueChatAction(
            "Resolve the queued second turn once the first finishes.",
            "send_message",
            """{"message":"The second turn resolves afterward."}""");

        var firstTurnTask = harness.PostTurnAsync(1, "inspect the vault");
        await firstRequestStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var secondTurnTask = harness.PostTurnAsync(2, "inspect the lock");
        await Task.Delay(200);

        harness.QwenOllama.CapturedChatRequestCount.Should().Be(1);
        secondTurnTask.IsCompleted.Should().BeFalse();

        releaseFirstTurn.SetResult(null);

        using var firstTurn = await firstTurnTask;
        using var secondTurn = await secondTurnTask;
        var firstPayload = await firstTurn.Content.ReadFromJsonAsync<TurnResponse>();
        var secondPayload = await secondTurn.Content.ReadFromJsonAsync<TurnResponse>();

        firstTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        secondTurn.StatusCode.Should().Be(HttpStatusCode.OK);
        firstPayload!.Text.Should().Be("The first turn resolves cleanly.");
        secondPayload!.Text.Should().Be("The second turn resolves afterward.");
        harness.QwenOllama.CapturedChatRequestCount.Should().Be(2);
    }

    private static string CombineContents(IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages)
    {
        return string.Join("\n", messages.Select(static message => message.Content));
    }
}
