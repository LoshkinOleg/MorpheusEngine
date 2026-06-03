using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
public sealed class EngineHttpContractsTests
{
    [Fact]
    // Verifies that known contract example templates exist and contain valid JSON where provided.
    public void EngineContractExamples_KnownTemplateIds_ReturnNonNullPairsWithValidJson()
    {
        foreach (var templateId in KnownTemplateIds)
        {
            var templates = EngineContractExamples.TryGetTemplates(templateId);

            templates.Should().NotBeNull($"template id '{templateId}' should be defined");

            if (templates!.RequestBodyTemplate is not null)
            {
                JsonNode.Parse(templates.RequestBodyTemplate).Should().NotBeNull();
            }

            if (templates.ResponseBodyTemplate is not null)
            {
                JsonNode.Parse(templates.ResponseBodyTemplate).Should().NotBeNull();
            }
        }
    }

    [Fact]
    // Verifies that all HTTP contract DTO samples round-trip through JSON serialization without payload drift.
    public void EngineHttpContracts_AllDtoRecords_RoundTripThroughJsonSerializer()
    {
        foreach (var sample in CreateRoundTripSamples())
        {
            var serialized = JsonSerializer.Serialize(sample.Payload, sample.Type);
            var deserialized = JsonSerializer.Deserialize(serialized, sample.Type);
            var reserialized = JsonSerializer.Serialize(deserialized, sample.Type);

            deserialized.Should().NotBeNull($"round-tripping {sample.Type.Name} should produce an object");
            JsonNode.DeepEquals(JsonNode.Parse(serialized), JsonNode.Parse(reserialized))
                .Should()
                .BeTrue($"round-tripping {sample.Type.Name} should preserve the JSON payload");
        }
    }

    [Fact]
    // Verifies that public wire properties declare camelCase JSON property names.
    public void EngineHttpContracts_PublicWireProperties_UseCamelCaseJsonPropertyNames()
    {
        foreach (var contractType in ContractTypes)
        {
            var properties = contractType.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.DeclaredOnly);
            properties.Should().NotBeEmpty($"{contractType.Name} should expose JSON-bound properties");

            foreach (var property in properties)
            {
                var attribute = property.GetCustomAttribute<JsonPropertyNameAttribute>();

                attribute.Should().NotBeNull($"{contractType.Name}.{property.Name} should declare JsonPropertyName");
                attribute!.Name.Should().NotBe(property.Name, "wire names should not fall back to PascalCase CLR member names");
                char.IsLower(attribute.Name[0]).Should().BeTrue($"{contractType.Name}.{property.Name} should use camelCase or lowercase wire names");
            }
        }
    }

    private static readonly string[] KnownTemplateIds =
    [
        "turn",
        "initialize",
        "persist_turn",
        "generate",
        "intent",
        "director_message",
        "chat",
        "memory_load_context",
        "memory_persist_step",
        "memory_recall_search",
        "memory_archival_search",
        "memory_archival_upsert",
        "memory_summaries_recent",
        "memory_recall_compact",
        "memory_blocks_get_all",
        "memory_blocks_upsert",
        "memory_messages_recent",
        "memory_messages_append",
        "memory_mutations_append",
        "memory_snapshot_latest",
        "memory_pipeline_events_recent",
        "embed",
        "token_count",
        "module_proxy",
        "module_info",
        "module_health",
        "module_shutdown",
        "llm_provider_last_llm_payload"
    ];

    private static readonly Type[] ContractTypes =
    [
        typeof(ModuleInfoResponse),
        typeof(ModuleHealthResponse),
        typeof(ModuleShutdownResponse),
        typeof(ErrorResponse),
        typeof(InitializeModuleRequest),
        typeof(InitializeModuleResponse),
        typeof(TurnRequest),
        typeof(TurnResponse),
        typeof(DirectorMessageRequest),
        typeof(DirectorMessageResponse),
        typeof(TurnPersistRequest),
        typeof(TurnPersistResponse),
        typeof(MemoryBlockDto),
        typeof(AgentMessageDto),
        typeof(MemoryMutationDto),
        typeof(LatestSnapshotDto),
        typeof(MemoryBudgetDto),
        typeof(MemorySummaryDto),
        typeof(MemoryContextItemDto),
        typeof(MemoryContextAccountingDto),
        typeof(MemoryLoadContextRequest),
        typeof(MemoryLoadContextResponse),
        typeof(MemoryPersistStepRequest),
        typeof(MemoryPersistStepResponse),
        typeof(MemoryRecallSearchRequest),
        typeof(MemorySearchResultDto),
        typeof(MemoryRecallSearchResponse),
        typeof(MemoryArchivalSearchRequest),
        typeof(MemoryArchivalSearchResponse),
        typeof(ArchivalPassageDto),
        typeof(MemoryArchivalUpsertRequest),
        typeof(MemoryArchivalUpsertResponse),
        typeof(MemorySummariesRecentRequest),
        typeof(MemorySummariesRecentResponse),
        typeof(MemoryCompactRecallRequest),
        typeof(MemoryCompactRecallResponse),
        typeof(MemoryBlocksGetAllRequest),
        typeof(MemoryBlocksGetAllResponse),
        typeof(MemoryBlockUpsertRequest),
        typeof(MemoryBlockUpsertResponse),
        typeof(MemoryMessagesRecentRequest),
        typeof(MemoryMessagesRecentResponse),
        typeof(MemoryMessageAppendRequest),
        typeof(MemoryMessageAppendResponse),
        typeof(MemoryMutationAppendRequest),
        typeof(MemoryMutationAppendResponse),
        typeof(MemorySnapshotLatestRequest),
        typeof(MemorySnapshotLatestResponse),
        typeof(MemoryPipelineEventsRecentRequest),
        typeof(MemoryPipelineEventDto),
        typeof(MemoryPipelineEventsRecentResponse),
        typeof(EmbeddingRequest),
        typeof(EmbeddingVectorDto),
        typeof(EmbeddingResponse),
        typeof(TokenCountRequest),
        typeof(TokenCountResponse),
        typeof(ModuleProxyRequest),
        typeof(IntentRequest),
        typeof(IntentResponse),
        typeof(LlmGenerateRequest),
        typeof(LlmProviderGenerateResponse),
        typeof(ChatGenerateRequest),
        typeof(ChatGenerateRequest.ChatMessageDto),
        typeof(ChatGenerateResponse),
        typeof(LlmProviderLastPayloadResponse)
    ];

    private static IReadOnlyList<(Type Type, object Payload)> CreateRoundTripSamples()
    {
        var block = new MemoryBlockDto("human", "Stable player-facing facts.", "Player prefers concise descriptions.", 2000, false);
        var agentMessage = new AgentMessageDto(1, 1, "assistant", "send_message", "You stand still and listen.", "send_message", "tool-1");
        var mutation = new MemoryMutationDto(1, 1, "core_memory_append", "human", null, "{\"append\":\"Player prefers concise descriptions.\"}");
        var snapshot = new LatestSnapshotDto(1, "{\"location\":\"dune\"}", "{\"visibleExits\":[\"north\"]}");
        var budget = new MemoryBudgetDto(4096, 2867, 12, 4000);
        var summary = new MemorySummaryDto(1, 6, "The party entered the ruin and learned the north door is sealed.", 18, "{\"reason\":\"budget\"}");
        var accountingItem = new MemoryContextItemDto("agent_prompt", "system", "included", 120, 120, "fits", 20, false);
        var accounting = new MemoryContextAccountingDto(480, 11468, ["none"], 4096, 2867, 120, false, [accountingItem]);
        var memorySearchResult = new MemorySearchResultDto("example-1", "The party entered the ruin.", "recall", 0.92, "{\"turn\":1}");
        var archivalPassage = new ArchivalPassageDto(
            "lore:default:ancient-ruins",
            "project",
            "lore/default_lore_entries.csv",
            "Ancient ruins contain sealed northern doors.",
            ["lore", "seed"],
            "{\"subject\":\"Ancient Ruins\"}",
            "nomic-embed-text",
            3,
            [0.12f, -0.04f, 0.88f]);
        var pipelineEvent = new MemoryPipelineEventDto(
            1,
            1,
            JsonSerializer.Serialize(new { eventType = "memory_context_budget", accounting }),
            "2026-05-02T08:00:00Z");
        var embeddingVector = new EmbeddingVectorDto(0, [0.12f, -0.04f, 0.88f]);
        var chatMessage = new ChatGenerateRequest.ChatMessageDto("user", "Look around.");

        return
        [
            (typeof(ModuleInfoResponse), new ModuleInfoResponse(true, "router")),
            (typeof(ModuleHealthResponse), new ModuleHealthResponse(true, "ok", true)),
            (typeof(ModuleShutdownResponse), new ModuleShutdownResponse(true, "Shutdown requested.")),
            (typeof(ErrorResponse), new ErrorResponse(false, "bad request", "details")),
            (typeof(InitializeModuleRequest), new InitializeModuleRequest("sandcrawler", "00000000-0000-0000-0000-000000000001")),
            (typeof(InitializeModuleResponse), new InitializeModuleResponse(true)),
            (typeof(TurnRequest), new TurnRequest(1, "look around")),
            (typeof(TurnResponse), new TurnResponse(true, "You stand still and listen.")),
            (typeof(DirectorMessageRequest), new DirectorMessageRequest(1, "Look around.")),
            (typeof(DirectorMessageResponse), new DirectorMessageResponse(true, "You stand still and listen.")),
            (typeof(TurnPersistRequest), new TurnPersistRequest(1, "look around", "{\"ok\":true,\"text\":\"You stand still and listen.\"}")),
            (typeof(TurnPersistResponse), new TurnPersistResponse(true)),
            (typeof(MemoryBlockDto), block),
            (typeof(AgentMessageDto), agentMessage),
            (typeof(MemoryMutationDto), mutation),
            (typeof(LatestSnapshotDto), snapshot),
            (typeof(MemoryBudgetDto), budget),
            (typeof(MemorySummaryDto), summary),
            (typeof(MemoryContextItemDto), accountingItem),
            (typeof(MemoryContextAccountingDto), accounting),
            (typeof(MemoryLoadContextRequest), new MemoryLoadContextRequest(1, 12)),
            (typeof(MemoryLoadContextResponse), new MemoryLoadContextResponse(true, [block], [agentMessage], snapshot, budget, [summary], accounting)),
            (typeof(MemoryPersistStepRequest), new MemoryPersistStepRequest(1, 1, [agentMessage], [mutation], [block], accounting)),
            (typeof(MemoryPersistStepResponse), new MemoryPersistStepResponse(true)),
            (typeof(MemoryRecallSearchRequest), new MemoryRecallSearchRequest("recent decisions", ["assistant"], 5)),
            (typeof(MemorySearchResultDto), memorySearchResult),
            (typeof(MemoryRecallSearchResponse), new MemoryRecallSearchResponse(true, [memorySearchResult])),
            (typeof(MemoryArchivalSearchRequest), new MemoryArchivalSearchRequest("ancient ruins", ["lore"], 5, [0.12f, -0.04f, 0.88f], "nomic-embed-text")),
            (typeof(MemoryArchivalSearchResponse), new MemoryArchivalSearchResponse(true, [memorySearchResult])),
            (typeof(ArchivalPassageDto), archivalPassage),
            (typeof(MemoryArchivalUpsertRequest), new MemoryArchivalUpsertRequest(archivalPassage)),
            (typeof(MemoryArchivalUpsertResponse), new MemoryArchivalUpsertResponse(true, archivalPassage)),
            (typeof(MemorySummariesRecentRequest), new MemorySummariesRecentRequest(5)),
            (typeof(MemorySummariesRecentResponse), new MemorySummariesRecentResponse(true, [summary])),
            (typeof(MemoryCompactRecallRequest), new MemoryCompactRecallRequest(1, 6, "The party entered the ruin and learned the north door is sealed.", 18, "{\"reason\":\"budget\"}")),
            (typeof(MemoryCompactRecallResponse), new MemoryCompactRecallResponse(true, summary)),
            (typeof(MemoryBlocksGetAllRequest), new MemoryBlocksGetAllRequest(true)),
            (typeof(MemoryBlocksGetAllResponse), new MemoryBlocksGetAllResponse(true, [block])),
            (typeof(MemoryBlockUpsertRequest), new MemoryBlockUpsertRequest(block)),
            (typeof(MemoryBlockUpsertResponse), new MemoryBlockUpsertResponse(true)),
            (typeof(MemoryMessagesRecentRequest), new MemoryMessagesRecentRequest(12, ["assistant", "tool"])),
            (typeof(MemoryMessagesRecentResponse), new MemoryMessagesRecentResponse(true, [agentMessage])),
            (typeof(MemoryMessageAppendRequest), new MemoryMessageAppendRequest(agentMessage)),
            (typeof(MemoryMessageAppendResponse), new MemoryMessageAppendResponse(true)),
            (typeof(MemoryMutationAppendRequest), new MemoryMutationAppendRequest(mutation)),
            (typeof(MemoryMutationAppendResponse), new MemoryMutationAppendResponse(true)),
            (typeof(MemorySnapshotLatestRequest), new MemorySnapshotLatestRequest(true)),
            (typeof(MemorySnapshotLatestResponse), new MemorySnapshotLatestResponse(true, snapshot)),
            (typeof(MemoryPipelineEventsRecentRequest), new MemoryPipelineEventsRecentRequest(10, "memory_context_budget")),
            (typeof(MemoryPipelineEventDto), pipelineEvent),
            (typeof(MemoryPipelineEventsRecentResponse), new MemoryPipelineEventsRecentResponse(true, [pipelineEvent])),
            (typeof(EmbeddingRequest), new EmbeddingRequest("nomic-embed-text", ["The party enters the ruin."])),
            (typeof(EmbeddingVectorDto), embeddingVector),
            (typeof(EmbeddingResponse), new EmbeddingResponse(true, "nomic-embed-text", 3, [embeddingVector])),
            (typeof(TokenCountRequest), new TokenCountRequest("qwen2.5:7b-instruct", "The party enters the ruin.")),
            (typeof(TokenCountResponse), new TokenCountResponse(true, "qwen2.5:7b-instruct", 7, false)),
            (typeof(ModuleProxyRequest), new ModuleProxyRequest(
                "intent_extractor",
                "generic_llm_provider",
                "/generate",
                "POST",
                JsonSerializer.SerializeToElement(new LlmGenerateRequest("Write a short response.")))),
            (typeof(IntentRequest), new IntentRequest("look around")),
            (typeof(IntentResponse), new IntentResponse(true, "look_around", new Dictionary<string, string> { ["direction"] = "around" })),
            (typeof(LlmGenerateRequest), new LlmGenerateRequest("Write a short response.", "You are a helpful assistant.")),
            (typeof(LlmProviderGenerateResponse), new LlmProviderGenerateResponse(true, "Short response.", "{\"raw\":\"...\"}")),
            (typeof(ChatGenerateRequest), new ChatGenerateRequest
            {
                Messages =
                [
                    new ChatGenerateRequest.ChatMessageDto("system", "You are the GM."),
                    chatMessage
                ],
                Format = JsonSerializer.SerializeToElement(new { type = "json_object" }),
                KeepAlive = "30m"
            }),
            (typeof(ChatGenerateRequest.ChatMessageDto), chatMessage),
            (typeof(ChatGenerateResponse), new ChatGenerateResponse(true, "You stand still and listen.", "{\"raw\":\"...\"}")),
            (typeof(LlmProviderLastPayloadResponse), new LlmProviderLastPayloadResponse(
                true,
                true,
                "chat",
                "2026-05-13T08:00:00Z",
                """{"model":"qwen2.5:7b-instruct","messages":[{"role":"user","content":"Look around."}]}"""))
        ];
    }
}
