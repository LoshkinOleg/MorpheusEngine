using System.Text.Json;
using FluentAssertions;
using Microsoft.Data.Sqlite;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;

namespace MorpheusEngine.Tests.Integration.SessionStore;

[Trait("Category", "Integration")]
[Collection("EngineProcessState")]
public sealed class RunPersistenceIntegrationTests
{
    private static readonly string[] ExpectedTables =
    [
        "meta",
        "events",
        "snapshots",
        "lore",
        "turn_execution",
        "pipeline_events",
        "memory_blocks",
        "agent_messages",
        "agent_messages_fts",
        "memory_mutations",
        "conversation_summaries",
        "archival_passages"
    ];

    private static readonly string[] ExpectedIndexes =
    [
        "idx_pipeline_events_turn_step",
        "idx_agent_messages_turn_step",
        "idx_agent_messages_role",
        "idx_memory_mutations_turn_step",
        "idx_conversation_summaries_turn_range",
        "idx_archival_passages_scope",
        "idx_archival_passages_source",
        "idx_archival_passages_created_at"
    ];

    private static readonly string[] ExpectedTriggers =
    [
        "agent_messages_ai",
        "agent_messages_ad",
        "agent_messages_au"
    ];

    [Fact]
    public void RunPersistence_InitializeRun_CreatesSchema()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            var response = persistence.InitializeRun("test_game", "test_run_001");
            var dbPath = BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001");

            response.Ok.Should().BeTrue();
            File.Exists(dbPath).Should().BeTrue();

            using var connection = OpenConnection(dbPath);

            ReadSqliteObjectNames(connection, "table").Should().Contain(ExpectedTables);
            ReadSqliteObjectNames(connection, "index").Should().Contain(ExpectedIndexes);
            ReadSqliteObjectNames(connection, "trigger").Should().Contain(ExpectedTriggers);
        });
    }

    [Fact]
    public void RunPersistence_InitializeRun_SetsMetaValues()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var meta = ReadMeta(connection);

            meta.Should().Contain(new KeyValuePair<string, string>("run_id", "test_run_001"));
            meta.Should().Contain(new KeyValuePair<string, string>("game_project_id", "test_game"));
        });
    }

    [Fact]
    public void RunPersistence_InitializeRun_InsertsTurnZeroSnapshotWithEmptyWorldAndViewState()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            using var command = connection.CreateCommand();
            command.CommandText =
                """
                SELECT turn, world_state, view_state
                FROM snapshots
                WHERE turn = 0
                ORDER BY id DESC
                LIMIT 1;
                """;

            using var reader = command.ExecuteReader();
            reader.Read().Should().BeTrue();
            reader.GetInt32(0).Should().Be(0);

            var worldState = reader.GetString(1);
            var viewState = reader.GetString(2);

            using var worldDocument = JsonDocument.Parse(worldState);
            var worldRoot = worldDocument.RootElement;
            worldRoot.GetProperty("gameProjectId").GetString().Should().Be("test_game");
            worldRoot.GetProperty("entities").EnumerateArray().Should().BeEmpty();
            worldRoot.GetProperty("facts").EnumerateArray().Should().BeEmpty();
            worldRoot.GetProperty("anchors").EnumerateArray().Should().BeEmpty();

            using var viewDocument = JsonDocument.Parse(viewState);
            viewDocument.RootElement
                .GetProperty("player")
                .GetProperty("observations")
                .EnumerateArray()
                .Should()
                .BeEmpty();
        });
    }

    [Fact]
    public void RunPersistence_InitializeRun_IsIdempotent()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            var firstResponse = persistence.InitializeRun("test_game", "test_run_001");
            var secondResponse = persistence.InitializeRun("test_game", "test_run_001");

            firstResponse.Ok.Should().BeTrue();
            secondResponse.Ok.Should().BeTrue();

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));

            ReadSqliteObjectNames(connection, "table").Should().Contain(ExpectedTables);
            ReadMeta(connection).Should().Contain(new KeyValuePair<string, string>("run_id", "test_run_001"));

            CountRows(connection, "snapshots", "turn = 0").Should().Be(1);
            CountRows(connection, "lore").Should().Be(2);
        });
    }

    [Fact]
    public void RunPersistence_InitializeRun_SeedsLoreFromDefaultLoreEntriesCsv()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var loreRows = ReadLoreRows(connection);

            loreRows.Should().BeEquivalentTo(
                new[]
                {
                    ("Ancient Ruins", "Crumbling structures in the northern desert.", "lore/default_lore_entries.csv"),
                    ("Oasis City", "A walled settlement around a freshwater spring.", "lore/default_lore_entries.csv")
                });
        });
    }

    [Fact]
    public void RunPersistence_InitializeRun_EmptyGameProjectId_ThrowsArgumentException()
    {
        WithConfiguredGameProject((persistence, _) =>
        {
            var act = () => persistence.InitializeRun(string.Empty, "test_run_001");

            act.Should().Throw<ArgumentException>()
                .WithParameterName("gameProjectId");
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_TurnOneAfterInitializeRun_SucceedsAndCreatesEventsAndSnapshot()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            var response = persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));

            response.Ok.Should().BeTrue();

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            CountRows(connection, "events").Should().Be(2);
            CountRows(connection, "snapshots", "turn = 1").Should().Be(1);
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_TurnTwoAfterTurnOne_Succeeds()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");
            persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));

            var response = persistence.PersistTurn(
                "test_game",
                "test_run_001",
                CreatePersistTurnRequest(turn: 2, playerInput: "open the door", directorResponseBody: """{"ok":true,"text":"The door creaks open."}"""));

            response.Ok.Should().BeTrue();

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            CountRows(connection, "events").Should().Be(4);
            CountRows(connection, "snapshots", "turn = 2").Should().Be(1);
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_TurnTwoWithoutTurnOne_ThrowsInvalidOperationException()
    {
        WithConfiguredGameProject((persistence, _) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            var act = () => persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 2));

            act.Should().Throw<InvalidOperationException>()
                .WithMessage("*Turn sequencing violation on persist*expected 1*");
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_TurnZero_ThrowsInvalidOperationException()
    {
        WithConfiguredGameProject((persistence, _) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            var act = () => persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 0));

            act.Should().Throw<InvalidOperationException>()
                .WithMessage("Turn must be >= 1.");
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_BeforeInitializeRun_ThrowsInvalidOperationException()
    {
        WithConfiguredGameProject((persistence, _) =>
        {
            var act = () => persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));

            act.Should().Throw<InvalidOperationException>()
                .WithMessage("*Run database not found*bind the run before persisting turns*");
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_InsertsPlayerInputAndModuleTraceEvents()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");
            persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var events = ReadEvents(connection);

            events.Should().HaveCount(2);
            events[0].Turn.Should().Be(1);
            events[0].EventType.Should().Be("player_input");
            events[1].Turn.Should().Be(1);
            events[1].EventType.Should().Be("module_trace");

            using var playerPayload = JsonDocument.Parse(events[0].Payload);
            playerPayload.RootElement.GetProperty("text").GetString().Should().Be("look around");

            using var tracePayload = JsonDocument.Parse(events[1].Payload);
            tracePayload.RootElement.GetProperty("narrationText").GetString().Should().Be("You stand still and listen.");
            tracePayload.RootElement.GetProperty("playerInputEcho").GetString().Should().Be("look around");
            tracePayload.RootElement.GetProperty("directorRaw").GetString().Should().Be("""{"ok":true,"text":"You stand still and listen."}""");
        });
    }

    [Fact]
    public void RunPersistence_PersistTurn_SnapshotCarriesForwardLatestWorldState()
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            persistence.InitializeRun("test_game", "test_run_001");

            using (var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001")))
            {
                const string customWorldState = """{"gameProjectId":"test_game","entities":[{"id":"door"}],"facts":["opened"],"anchors":[]}""";
                UpdateSnapshotWorldState(connection, turn: 0, customWorldState);
            }

            persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));

            using var verificationConnection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var snapshot = ReadSnapshotForTurn(verificationConnection, turn: 1);

            using var worldState = JsonDocument.Parse(snapshot.WorldState);
            worldState.RootElement.GetProperty("entities")[0].GetProperty("id").GetString().Should().Be("door");
            worldState.RootElement.GetProperty("facts")[0].GetString().Should().Be("opened");
        });
    }

    [Fact]
    public void RunPersistence_UpsertMemoryBlock_ThenGetMemoryBlocks_RoundTripsCorrectly()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            var block = CreateMemoryBlock(label: "human", value: "Player prefers concise descriptions.", readOnly: false);

            var response = persistence.UpsertMemoryBlock("test_game", "test_run_001", new MemoryBlockUpsertRequest(block));
            var blocks = persistence.GetMemoryBlocks("test_game", "test_run_001", new MemoryBlocksGetAllRequest(true));

            response.Ok.Should().BeTrue();
            blocks.Ok.Should().BeTrue();
            blocks.Blocks.Should().ContainEquivalentOf(block);
        });
    }

    [Fact]
    public void RunPersistence_UpsertMemoryBlock_UpdatesExistingBlock()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.UpsertMemoryBlock(
                "test_game",
                "test_run_001",
                new MemoryBlockUpsertRequest(CreateMemoryBlock(label: "human", value: "Original memory.", readOnly: false)));

            var updatedBlock = CreateMemoryBlock(label: "human", description: "Updated description.", value: "Updated memory.", readOnly: false);
            var response = persistence.UpsertMemoryBlock("test_game", "test_run_001", new MemoryBlockUpsertRequest(updatedBlock));
            var blocks = persistence.GetMemoryBlocks("test_game", "test_run_001", new MemoryBlocksGetAllRequest(true));

            response.Ok.Should().BeTrue();
            blocks.Blocks.Should().ContainSingle(block => block.Label == "human")
                .Which.Should().Be(updatedBlock);
        });
    }

    [Fact]
    public void RunPersistence_GetMemoryBlocks_IncludeReadOnlyFalse_ExcludesReadOnlyBlocks()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.UpsertMemoryBlock(
                "test_game",
                "test_run_001",
                new MemoryBlockUpsertRequest(CreateMemoryBlock(label: "human", readOnly: false)));
            persistence.UpsertMemoryBlock(
                "test_game",
                "test_run_001",
                new MemoryBlockUpsertRequest(CreateMemoryBlock(label: "system", readOnly: true)));

            var response = persistence.GetMemoryBlocks("test_game", "test_run_001", new MemoryBlocksGetAllRequest(false));

            response.Ok.Should().BeTrue();
            response.Blocks.Select(block => block.Label).Should().BeEquivalentTo(["human"]);
        });
    }

    [Fact]
    public void RunPersistence_AppendMessage_ThenGetRecentMessages_ReturnsIt()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            var message = CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "You stand still and listen.");

            var appendResponse = persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(message));
            var recentResponse = persistence.GetRecentMessages("test_game", "test_run_001", new MemoryMessagesRecentRequest(12));

            appendResponse.Ok.Should().BeTrue();
            recentResponse.Ok.Should().BeTrue();
            recentResponse.Messages.Should().ContainEquivalentOf(message);
        });
    }

    [Fact]
    public void RunPersistence_GetRecentMessages_RespectsLimit()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "First")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 1, stepNumber: 1, role: "assistant", content: "Second")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 2, stepNumber: 0, role: "assistant", content: "Third")));

            var response = persistence.GetRecentMessages("test_game", "test_run_001", new MemoryMessagesRecentRequest(2));

            response.Ok.Should().BeTrue();
            response.Messages.Should().HaveCount(2);
            response.Messages.Select(message => message.Content).Should().Equal("Second", "Third");
        });
    }

    [Fact]
    public void RunPersistence_GetRecentMessages_WithRoleFilter_ReturnsOnlyMatchingRoles()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "Assistant response")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 1, stepNumber: 1, role: "tool", content: "Tool output", messageType: "tool_result")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(CreateAgentMessage(turn: 2, stepNumber: 0, role: "user", content: "Player question", messageType: "player_input")));

            var response = persistence.GetRecentMessages("test_game", "test_run_001", new MemoryMessagesRecentRequest(10, ["assistant", "tool"]));

            response.Ok.Should().BeTrue();
            response.Messages.Select(message => message.Role).Should().BeEquivalentTo(["assistant", "tool"]);
        });
    }

    [Fact]
    public void RunPersistence_AppendMutation_PersistsMutation()
    {
        WithInitializedRun((persistence, gameProject, _) =>
        {
            var mutation = CreateMemoryMutation(turn: 1, stepNumber: 0);

            var response = persistence.AppendMutation("test_game", "test_run_001", new MemoryMutationAppendRequest(mutation));

            response.Ok.Should().BeTrue();

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var mutations = ReadMemoryMutations(connection);
            mutations.Should().ContainSingle().Which.Should().Be(mutation);
        });
    }

    [Fact]
    public void RunPersistence_GetLatestSnapshot_ReturnsMostRecentSnapshotByTurn()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 1));
            persistence.PersistTurn("test_game", "test_run_001", CreatePersistTurnRequest(turn: 2, playerInput: "open the door", directorResponseBody: """{"ok":true,"text":"The door creaks open."}"""));

            var response = persistence.GetLatestSnapshot("test_game", "test_run_001");

            response.Ok.Should().BeTrue();
            response.Snapshot.Turn.Should().Be(2);
        });
    }

    [Fact]
    public void RunPersistence_PersistMemoryStep_WritesMessagesMutationsBlocksAndPipelineEventAtomically()
    {
        WithInitializedRun((persistence, gameProject, _) =>
        {
            var request = new MemoryPersistStepRequest(
                1,
                2,
                [
                    CreateAgentMessage(turn: 1, stepNumber: 2, role: "assistant", content: "Assistant message"),
                    CreateAgentMessage(turn: 1, stepNumber: 2, role: "tool", content: "Tool result", messageType: "tool_result")
                ],
                [
                    CreateMemoryMutation(turn: 1, stepNumber: 2)
                ],
                [
                    CreateMemoryBlock(label: "human", value: "Updated by memory step.", readOnly: false)
                ],
                CreateContextAccounting());

            var response = persistence.PersistMemoryStep("test_game", "test_run_001", request);

            response.Ok.Should().BeTrue();

            var messages = persistence.GetRecentMessages("test_game", "test_run_001", new MemoryMessagesRecentRequest(10));
            var blocks = persistence.GetMemoryBlocks("test_game", "test_run_001", new MemoryBlocksGetAllRequest(true));

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var mutations = ReadMemoryMutations(connection);
            var pipelineEvents = ReadPipelineEvents(connection);

            messages.Messages.Should().ContainEquivalentOf(request.Messages[0]);
            messages.Messages.Should().ContainEquivalentOf(request.Messages[1]);
            blocks.Blocks.Should().ContainEquivalentOf(request.BlockUpdates[0]);
            mutations.Should().ContainEquivalentOf(request.Mutations[0]);
            pipelineEvents.Should().ContainSingle();
            pipelineEvents[0].Turn.Should().Be(1);
            pipelineEvents[0].StepNumber.Should().Be(2);

            using var payload = JsonDocument.Parse(pipelineEvents[0].PayloadJson);
            payload.RootElement.GetProperty("eventType").GetString().Should().Be("memory_context_budget");
        });
    }

    [Fact]
    public void RunPersistence_SearchRecall_ReturnsMatchingMessagesRankedByRelevance()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "Ancient ruins ancient ruins hidden temple")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 1, role: "assistant", content: "Ancient temple door")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 2, role: "assistant", content: "Completely unrelated market square")));

            var response = persistence.SearchRecall("test_game", "test_run_001", new MemoryRecallSearchRequest("ancient ruins", null, 5));

            response.Ok.Should().BeTrue();
            response.Results.Should().HaveCount(2);
            response.Results[0].Content.Should().Be("Ancient ruins ancient ruins hidden temple");
            response.Results[1].Content.Should().Be("Ancient temple door");
            response.Results[0].Score.Should().NotBeNull();
            response.Results[1].Score.Should().NotBeNull();
            response.Results[0].Score!.Value.Should().BeLessThan(response.Results[1].Score!.Value);
        });
    }

    [Fact]
    public void RunPersistence_SearchRecall_WithRoleFilter_AppliesCorrectly()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "Ancient ruins response")));
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 1, role: "tool", content: "Ancient ruins tool output", messageType: "tool_result")));

            var response = persistence.SearchRecall("test_game", "test_run_001", new MemoryRecallSearchRequest("ancient ruins", ["tool"], 5));

            response.Ok.Should().BeTrue();
            response.Results.Should().ContainSingle();
            response.Results[0].Content.Should().Be("Ancient ruins tool output");
        });
    }

    [Fact]
    public void RunPersistence_SearchRecall_NoMatches_ReturnsEmptyList()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
                CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "Ancient ruins response")));

            var response = persistence.SearchRecall("test_game", "test_run_001", new MemoryRecallSearchRequest("spaceship", null, 5));

            response.Ok.Should().BeTrue();
            response.Results.Should().BeEmpty();
        });
    }

    [Fact]
    public void RunPersistence_SearchArchival_ReturnsTopKOrderedByCosineSimilarity()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.UpsertArchivalPassage("test_game", "test_run_001", new MemoryArchivalUpsertRequest(
                CreateArchivalPassage(id: "passage-best", content: "Ancient ruins best match", tags: ["lore"], embedding: [1f, 0f, 0f])));
            persistence.UpsertArchivalPassage("test_game", "test_run_001", new MemoryArchivalUpsertRequest(
                CreateArchivalPassage(id: "passage-mid", content: "Ancient ruins weaker match", tags: ["lore"], embedding: [0.6f, 0.8f, 0f])));
            persistence.UpsertArchivalPassage("test_game", "test_run_001", new MemoryArchivalUpsertRequest(
                CreateArchivalPassage(id: "passage-low", content: "Ancient ruins opposite vector", tags: ["lore"], embedding: [-1f, 0f, 0f])));

            var response = persistence.SearchArchival(
                "test_game",
                "test_run_001",
                new MemoryArchivalSearchRequest("ancient ruins", null, 2, [1f, 0f, 0f], "nomic-embed-text"));

            response.Ok.Should().BeTrue();
            response.Results.Should().HaveCount(2);
            response.Results[0].Id.Should().Be("passage-best");
            response.Results[1].Id.Should().Be("passage-mid");
            response.Results[0].Score.Should().NotBeNull();
            response.Results[1].Score.Should().NotBeNull();
            response.Results[0].Score!.Value.Should().BeGreaterThan(response.Results[1].Score!.Value);
        });
    }

    [Fact]
    public void RunPersistence_SearchArchival_WithTagFilter_RestrictsResults()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.UpsertArchivalPassage("test_game", "test_run_001", new MemoryArchivalUpsertRequest(
                CreateArchivalPassage(id: "passage-lore", tags: ["lore", "seed"], embedding: [1f, 0f, 0f])));
            persistence.UpsertArchivalPassage("test_game", "test_run_001", new MemoryArchivalUpsertRequest(
                CreateArchivalPassage(id: "passage-journal", tags: ["journal"], embedding: [0.9f, 0.1f, 0f])));

            var response = persistence.SearchArchival(
                "test_game",
                "test_run_001",
                new MemoryArchivalSearchRequest("ancient", ["lore"], 5, [1f, 0f, 0f], "nomic-embed-text"));

            response.Ok.Should().BeTrue();
            response.Results.Select(result => result.Id).Should().Equal("passage-lore");
        });
    }

    [Fact]
    public void RunPersistence_UpsertArchivalPassage_ValidPassage_InsertsAndUpdates()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            var inserted = persistence.UpsertArchivalPassage(
                "test_game",
                "test_run_001",
                new MemoryArchivalUpsertRequest(CreateArchivalPassage(
                    id: "passage-001",
                    scope: "PROJECT",
                    tags: ["Lore", "Seed"],
                    content: "First content")));

            var updated = persistence.UpsertArchivalPassage(
                "test_game",
                "test_run_001",
                new MemoryArchivalUpsertRequest(CreateArchivalPassage(
                    id: "passage-001",
                    scope: "run",
                    tags: ["journal"],
                    content: "Updated content",
                    embedding: [0.5f, 0.5f, 0f])));

            inserted.Ok.Should().BeTrue();
            inserted.Passage.Scope.Should().Be("project");
            inserted.Passage.Tags.Should().Equal("lore", "seed");

            updated.Ok.Should().BeTrue();
            updated.Passage.Scope.Should().Be("run");
            updated.Passage.Content.Should().Be("Updated content");
            updated.Passage.Tags.Should().Equal("journal");
            updated.Passage.EmbeddingDimensions.Should().Be(3);

            var searchResponse = persistence.SearchArchival(
                "test_game",
                "test_run_001",
                new MemoryArchivalSearchRequest("updated", null, 5, [0.5f, 0.5f, 0f], "nomic-embed-text"));
            searchResponse.Results.Should().ContainSingle(result => result.Id == "passage-001" && result.Content == "Updated content");
        });
    }

    [Fact]
    public void RunPersistence_UpsertArchivalPassage_InvalidPassage_Throws()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            var emptyIdAct = () => persistence.UpsertArchivalPassage(
                "test_game",
                "test_run_001",
                new MemoryArchivalUpsertRequest(CreateArchivalPassage(id: "   ")));
            var dimensionMismatchAct = () => persistence.UpsertArchivalPassage(
                "test_game",
                "test_run_001",
                new MemoryArchivalUpsertRequest(new ArchivalPassageDto(
                    "passage-bad",
                    "project",
                    "lore/default_lore_entries.csv",
                    "Bad dimensions",
                    ["lore"],
                    """{"subject":"Bad"}""",
                    "nomic-embed-text",
                    4,
                    [1f, 0f, 0f])));

            emptyIdAct.Should().Throw<InvalidOperationException>()
                .WithMessage("Archival passage id must be non-empty.");
            dimensionMismatchAct.Should().Throw<InvalidOperationException>()
                .WithMessage("*embeddingDimensions must match*");
        });
    }

    [Fact]
    public void RunPersistence_BuildArchivalLoreSeedCandidates_ConvertsCsvRowsToCandidates()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            var candidates = persistence.BuildArchivalLoreSeedCandidates("test_game", "test_run_001");

            candidates.Should().HaveCount(2);
            candidates[0].Scope.Should().Be("project");
            candidates[0].Source.Should().Be("lore/default_lore_entries.csv");
            candidates[0].Tags.Should().Equal("lore", "seed");
            candidates[0].Content.Should().StartWith("Ancient Ruins: ");
            candidates[0].Id.Should().StartWith("lore:default:");

            using var metadata = JsonDocument.Parse(candidates[0].MetadataJson);
            metadata.RootElement.GetProperty("subject").GetString().Should().Be("Ancient Ruins");
            metadata.RootElement.GetProperty("source").GetString().Should().Be("lore/default_lore_entries.csv");
        });
    }

    [Fact]
    public void RunPersistence_CompactRecall_InsertsConversationSummaryWithCorrectTurnRange()
    {
        WithInitializedRun((persistence, gameProject, _) =>
        {
            SeedMessagesForCompaction(persistence);

            var response = persistence.CompactRecall(
                "test_game",
                "test_run_001",
                new MemoryCompactRecallRequest(1, 2, "Summary for turns 1-2", 3, """{"reason":"budget"}"""));

            response.Ok.Should().BeTrue();
            response.Summary.StartTurn.Should().Be(1);
            response.Summary.EndTurn.Should().Be(2);
            response.Summary.Summary.Should().Be("Summary for turns 1-2");

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            var summaries = ReadConversationSummaries(connection);
            summaries.Should().ContainSingle();
            summaries[0].StartTurn.Should().Be(1);
            summaries[0].EndTurn.Should().Be(2);
            summaries[0].Summary.Should().Be("Summary for turns 1-2");
        });
    }

    [Fact]
    public void RunPersistence_CompactRecall_DeletesCompactedAgentMessagesRows()
    {
        WithInitializedRun((persistence, gameProject, _) =>
        {
            SeedMessagesForCompaction(persistence);

            persistence.CompactRecall(
                "test_game",
                "test_run_001",
                new MemoryCompactRecallRequest(1, 2, "Summary for turns 1-2", 3, """{"reason":"budget"}"""));

            using var connection = OpenConnection(BuildDbPath(gameProject.RepositoryRoot, "test_game", "test_run_001"));
            CountRows(connection, "agent_messages", "turn BETWEEN 1 AND 2").Should().Be(0);
            CountRows(connection, "agent_messages", "turn = 3").Should().Be(1);
        });
    }

    [Fact]
    public void RunPersistence_GetRecentSummaries_ReturnsMostRecentSummariesLimitedByCount()
    {
        WithInitializedRun((persistence, _, _) =>
        {
            persistence.CompactRecall("test_game", "test_run_001", new MemoryCompactRecallRequest(1, 1, "Summary 1", 1, null));
            persistence.CompactRecall("test_game", "test_run_001", new MemoryCompactRecallRequest(2, 2, "Summary 2", 1, null));
            persistence.CompactRecall("test_game", "test_run_001", new MemoryCompactRecallRequest(3, 3, "Summary 3", 1, null));

            var response = persistence.GetRecentSummaries("test_game", "test_run_001", new MemorySummariesRecentRequest(2));

            response.Ok.Should().BeTrue();
            response.Summaries.Should().HaveCount(2);
            response.Summaries.Select(summary => summary.Summary).Should().Equal("Summary 3", "Summary 2");
        });
    }

    private static void WithConfiguredGameProject(Action<RunPersistence, TempGameProject> assertion)
    {
        using var gameProject = new TempGameProject(
            "test_game",
            TestPayloads.MinimalManifestJson,
            TestPayloads.MinimalLoreCsv,
            TestPayloads.MinimalSystemInstructions,
            TestPayloads.BuildMinimalEngineConfigJson());

        var originalCurrentDirectory = Environment.CurrentDirectory;
        Environment.CurrentDirectory = gameProject.RepositoryRoot;
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(gameProject.RepositoryRoot);

        try
        {
            var persistence = new RunPersistence(gameProject.RepositoryRoot);
            assertion(persistence, gameProject);
        }
        finally
        {
            EngineConfigLoader.ResetForTesting();
            Environment.CurrentDirectory = originalCurrentDirectory;
        }
    }

    private static void WithInitializedRun(Action<RunPersistence, TempGameProject, string> assertion)
    {
        WithConfiguredGameProject((persistence, gameProject) =>
        {
            const string runId = "test_run_001";
            persistence.InitializeRun("test_game", runId);
            assertion(persistence, gameProject, runId);
        });
    }

    private static string BuildDbPath(string repositoryRoot, string gameProjectId, string runId)
    {
        return Path.Combine(repositoryRoot, "game_projects", gameProjectId, "saved", runId, "world_state.db");
    }

    private static SqliteConnection OpenConnection(string dbPath)
    {
        var connection = new SqliteConnection($"Data Source={dbPath}");
        connection.Open();
        return connection;
    }

    private static HashSet<string> ReadSqliteObjectNames(SqliteConnection connection, string type)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT name
            FROM sqlite_master
            WHERE type = @type AND name NOT LIKE 'sqlite_%';
            """;
        command.Parameters.AddWithValue("@type", type);

        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            names.Add(reader.GetString(0));
        }

        return names;
    }

    private static Dictionary<string, string> ReadMeta(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT key, value FROM meta;";

        var meta = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            meta[reader.GetString(0)] = reader.GetString(1);
        }

        return meta;
    }

    private static int CountRows(SqliteConnection connection, string tableName, string? whereClause = null)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"SELECT COUNT(*) FROM {tableName}" + (string.IsNullOrWhiteSpace(whereClause) ? ";" : $" WHERE {whereClause};");
        return Convert.ToInt32(command.ExecuteScalar(), System.Globalization.CultureInfo.InvariantCulture);
    }

    private static IReadOnlyList<(string Subject, string Data, string Source)> ReadLoreRows(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT subject, data, source
            FROM lore
            ORDER BY subject COLLATE NOCASE;
            """;

        var rows = new List<(string Subject, string Data, string Source)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetString(0), reader.GetString(1), reader.GetString(2)));
        }

        return rows;
    }

    private static TurnPersistRequest CreatePersistTurnRequest(
        int turn,
        string playerInput = "look around",
        string directorResponseBody = """{"ok":true,"text":"You stand still and listen."}""")
    {
        return new TurnPersistRequest(turn, playerInput, directorResponseBody);
    }

    private static IReadOnlyList<(int Turn, string EventType, string Payload)> ReadEvents(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, event_type, payload
            FROM events
            ORDER BY id ASC;
            """;

        var rows = new List<(int Turn, string EventType, string Payload)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetInt32(0), reader.GetString(1), reader.GetString(2)));
        }

        return rows;
    }

    private static void UpdateSnapshotWorldState(SqliteConnection connection, int turn, string worldState)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            UPDATE snapshots
            SET world_state = @worldState
            WHERE turn = @turn;
            """;
        command.Parameters.AddWithValue("@worldState", worldState);
        command.Parameters.AddWithValue("@turn", turn);
        command.ExecuteNonQuery().Should().Be(1);
    }

    private static (string WorldState, string ViewState) ReadSnapshotForTurn(SqliteConnection connection, int turn)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT world_state, view_state
            FROM snapshots
            WHERE turn = @turn
            ORDER BY id DESC
            LIMIT 1;
            """;
        command.Parameters.AddWithValue("@turn", turn);

        using var reader = command.ExecuteReader();
        reader.Read().Should().BeTrue();
        return (reader.GetString(0), reader.GetString(1));
    }

    private static IReadOnlyList<MemoryMutationDto> ReadMemoryMutations(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, step_number, tool_name, target, before_json, after_json
            FROM memory_mutations
            ORDER BY id ASC;
            """;

        var rows = new List<MemoryMutationDto>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new MemoryMutationDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                reader.GetString(2),
                reader.GetString(3),
                reader.IsDBNull(4) ? null : reader.GetString(4),
                reader.IsDBNull(5) ? null : reader.GetString(5)));
        }

        return rows;
    }

    private static IReadOnlyList<(int Turn, int StepNumber, string PayloadJson)> ReadPipelineEvents(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, step_number, payload
            FROM pipeline_events
            ORDER BY id ASC;
            """;

        var rows = new List<(int Turn, int StepNumber, string PayloadJson)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetInt32(0), reader.GetInt32(1), reader.GetString(2)));
        }

        return rows;
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

    private static AgentMessageDto CreateAgentMessage(
        int turn,
        int stepNumber,
        string role,
        string content,
        string messageType = "send_message",
        string? toolName = null,
        string? toolCallId = null)
    {
        return new AgentMessageDto(turn, stepNumber, role, messageType, content, toolName, toolCallId);
    }

    private static MemoryMutationDto CreateMemoryMutation(
        int turn,
        int stepNumber,
        string toolName = "core_memory_append",
        string target = "human",
        string? beforeJson = null,
        string? afterJson = "{\"append\":\"Player prefers concise descriptions.\"}")
    {
        return new MemoryMutationDto(turn, stepNumber, toolName, target, beforeJson, afterJson);
    }

    private static MemoryContextAccountingDto CreateContextAccounting()
    {
        return new MemoryContextAccountingDto(
            480,
            11468,
            [],
            4096,
            2867,
            120,
            false,
            [
                new MemoryContextItemDto("agent_prompt", "system", "included", 120, 120)
            ]);
    }

    private static ArchivalPassageDto CreateArchivalPassage(
        string id,
        string scope = "project",
        string source = "lore/default_lore_entries.csv",
        string content = "Ancient ruins contain sealed northern doors.",
        IReadOnlyList<string>? tags = null,
        string? metadataJson = """{"subject":"Ancient Ruins"}""",
        string embeddingModel = "nomic-embed-text",
        IReadOnlyList<float>? embedding = null)
    {
        var vector = embedding ?? [0.12f, -0.04f, 0.88f];
        return new ArchivalPassageDto(
            id,
            scope,
            source,
            content,
            tags ?? ["lore", "seed"],
            metadataJson,
            embeddingModel,
            vector.Count,
            vector);
    }

    private static void SeedMessagesForCompaction(RunPersistence persistence)
    {
        persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
            CreateAgentMessage(turn: 1, stepNumber: 0, role: "assistant", content: "Turn 1 summary candidate")));
        persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
            CreateAgentMessage(turn: 2, stepNumber: 0, role: "assistant", content: "Turn 2 summary candidate")));
        persistence.AppendMessage("test_game", "test_run_001", new MemoryMessageAppendRequest(
            CreateAgentMessage(turn: 3, stepNumber: 0, role: "assistant", content: "Turn 3 should remain")));
    }

    private static IReadOnlyList<MemorySummaryDto> ReadConversationSummaries(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT start_turn, end_turn, summary, source_message_count, metadata_json
            FROM conversation_summaries
            ORDER BY end_turn ASC, start_turn ASC, id ASC;
            """;

        var rows = new List<MemorySummaryDto>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new MemorySummaryDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                reader.GetString(2),
                reader.GetInt32(3),
                reader.IsDBNull(4) ? null : reader.GetString(4)));
        }

        return rows;
    }
}
