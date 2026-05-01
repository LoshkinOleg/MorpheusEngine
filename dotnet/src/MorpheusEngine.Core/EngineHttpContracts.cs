using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

// Note: C# members use PascalCase; JSON wire names on these contracts use camelCase (JsonPropertyName). "params" stays as one word (not snake_case).

#region Module lifecycle (GET /info, /health; POST /shutdown)
public sealed record ModuleInfoResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("moduleName")] string ModuleName);

/// <summary>GET /health JSON. <see cref="Initialized"/> is false while awaiting or processing POST /initialize; true only when module-specific init is complete.
/// Status initialize_failed means a deferred bind (for example after POST /initialize returned 202) could not complete; the host should fail startup.
/// Status ollama_starting is used while the HTTP listener is up but bundled Ollama is not HTTP-ready yet (expect 200 from llm_provider_qwen so the host listen-wait can succeed).
/// Status ollama_startup_failed means bundled Ollama failed during initial bootstrap; the host should fail startup.</summary>
public sealed record ModuleHealthResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("status")] string Status,
    [property: JsonPropertyName("initialized")] bool Initialized);

public sealed record ModuleShutdownResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("message")] string Message);
#endregion

#region Errors (common JSON error envelope)
public sealed record ErrorResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("error")] string Error, // Short msg, not HTTP response code.
    [property: JsonPropertyName("details")] string? Details = null);
#endregion

#region Host POST /initialize payload (run binding)
public sealed record InitializeModuleRequest(
    [property: JsonPropertyName("gameProjectId")] string GameProjectId, // Needed.
    [property: JsonPropertyName("runId")] string RunId); // Needed.

public sealed record InitializeModuleResponse([property: JsonPropertyName("ok")] bool Ok);
#endregion

#region Turn pipeline (router POST /turn; director POST /message; session_store POST /persist_turn)
/// <summary>Player-facing turn envelope. Run identity comes from router process state bound by host POST /initialize.</summary>
public sealed record TurnRequest(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerInput")] string PlayerInput);

/// <summary>Router-owned response envelope returned by POST /turn.</summary>
public sealed record TurnResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("text")] string Text);

/// <summary>Router forwards to director POST /message after the host POST /initialize (single bound run per Director process).</summary>
public sealed record DirectorMessageRequest(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerInput")] string PlayerInput);

/// <summary>Director module response envelope returned by POST /message.</summary>
public sealed record DirectorMessageResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("text")] string Text);

/// <summary>Body for session_store POST /persist_turn; run identity comes from the last successful host POST /initialize on that module process.</summary>
public sealed record TurnPersistRequest(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerInput")] string PlayerInput,
    [property: JsonPropertyName("directorResponseBody")] string DirectorResponseBody);

public sealed record TurnPersistResponse(
    [property: JsonPropertyName("ok")] bool Ok);
#endregion

#region Memory-director Phase 0 storage and provider contracts
public sealed record MemoryBlockDto(
    [property: JsonPropertyName("label")] string Label,
    [property: JsonPropertyName("description")] string Description,
    [property: JsonPropertyName("value")] string Value,
    [property: JsonPropertyName("charLimit")] int CharLimit,
    [property: JsonPropertyName("readOnly")] bool ReadOnly);

public sealed record AgentMessageDto(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("stepNumber")] int StepNumber,
    [property: JsonPropertyName("role")] string Role,
    [property: JsonPropertyName("messageType")] string MessageType,
    [property: JsonPropertyName("content")] string Content,
    [property: JsonPropertyName("toolName")] string? ToolName = null,
    [property: JsonPropertyName("toolCallId")] string? ToolCallId = null);

public sealed record MemoryMutationDto(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("stepNumber")] int StepNumber,
    [property: JsonPropertyName("toolName")] string ToolName,
    [property: JsonPropertyName("target")] string Target,
    [property: JsonPropertyName("beforeJson")] string? BeforeJson = null,
    [property: JsonPropertyName("afterJson")] string? AfterJson = null);

public sealed record LatestSnapshotDto(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("worldStateJson")] string WorldStateJson,
    [property: JsonPropertyName("viewStateJson")] string ViewStateJson);

public sealed record MemoryBudgetDto(
    [property: JsonPropertyName("numCtx")] int NumCtx,
    [property: JsonPropertyName("targetContextTokens")] int TargetContextTokens,
    [property: JsonPropertyName("recentMessageCount")] int RecentMessageCount,
    [property: JsonPropertyName("maxToolResultChars")] int MaxToolResultChars);

public sealed record MemoryLoadContextRequest(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("recentMessageCount")] int RecentMessageCount = 12);

public sealed record MemoryLoadContextResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("blocks")] IReadOnlyList<MemoryBlockDto> Blocks,
    [property: JsonPropertyName("recentMessages")] IReadOnlyList<AgentMessageDto> RecentMessages,
    [property: JsonPropertyName("latestSnapshot")] LatestSnapshotDto LatestSnapshot,
    [property: JsonPropertyName("budget")] MemoryBudgetDto Budget);

public sealed record MemoryPersistStepRequest(
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("stepNumber")] int StepNumber,
    [property: JsonPropertyName("messages")] IReadOnlyList<AgentMessageDto> Messages,
    [property: JsonPropertyName("mutations")] IReadOnlyList<MemoryMutationDto> Mutations,
    [property: JsonPropertyName("blockUpdates")] IReadOnlyList<MemoryBlockDto> BlockUpdates);

public sealed record MemoryPersistStepResponse([property: JsonPropertyName("ok")] bool Ok);

public sealed record MemoryRecallSearchRequest(
    [property: JsonPropertyName("query")] string Query,
    [property: JsonPropertyName("roles")] IReadOnlyList<string>? Roles = null,
    [property: JsonPropertyName("limit")] int Limit = 5);

public sealed record MemorySearchResultDto(
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("content")] string Content,
    [property: JsonPropertyName("source")] string Source,
    [property: JsonPropertyName("score")] double? Score = null,
    [property: JsonPropertyName("metadataJson")] string? MetadataJson = null);

public sealed record MemoryRecallSearchResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("results")] IReadOnlyList<MemorySearchResultDto> Results);

public sealed record MemoryArchivalSearchRequest(
    [property: JsonPropertyName("query")] string Query,
    [property: JsonPropertyName("tags")] IReadOnlyList<string>? Tags = null,
    [property: JsonPropertyName("topK")] int TopK = 5);

public sealed record MemoryArchivalSearchResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("results")] IReadOnlyList<MemorySearchResultDto> Results);

public sealed record MemoryBlocksGetAllRequest([property: JsonPropertyName("includeReadOnly")] bool IncludeReadOnly = true);

public sealed record MemoryBlocksGetAllResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("blocks")] IReadOnlyList<MemoryBlockDto> Blocks);

public sealed record MemoryBlockUpsertRequest([property: JsonPropertyName("block")] MemoryBlockDto Block);

public sealed record MemoryBlockUpsertResponse([property: JsonPropertyName("ok")] bool Ok);

public sealed record MemoryMessagesRecentRequest(
    [property: JsonPropertyName("limit")] int Limit = 12,
    [property: JsonPropertyName("roles")] IReadOnlyList<string>? Roles = null);

public sealed record MemoryMessagesRecentResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("messages")] IReadOnlyList<AgentMessageDto> Messages);

public sealed record MemoryMessageAppendRequest([property: JsonPropertyName("message")] AgentMessageDto Message);

public sealed record MemoryMessageAppendResponse([property: JsonPropertyName("ok")] bool Ok);

public sealed record MemoryMutationAppendRequest([property: JsonPropertyName("mutation")] MemoryMutationDto Mutation);

public sealed record MemoryMutationAppendResponse([property: JsonPropertyName("ok")] bool Ok);

public sealed record MemorySnapshotLatestRequest([property: JsonPropertyName("includeViewState")] bool IncludeViewState = true);

public sealed record MemorySnapshotLatestResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("snapshot")] LatestSnapshotDto Snapshot);

public sealed record EmbeddingRequest(
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("texts")] IReadOnlyList<string> Texts);

public sealed record EmbeddingVectorDto(
    [property: JsonPropertyName("index")] int Index,
    [property: JsonPropertyName("vector")] IReadOnlyList<float> Vector);

public sealed record EmbeddingResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("dimensions")] int Dimensions,
    [property: JsonPropertyName("vectors")] IReadOnlyList<EmbeddingVectorDto> Vectors);

public sealed record TokenCountRequest(
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("text")] string Text);

public sealed record TokenCountResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("estimatedTokens")] int EstimatedTokens,
    [property: JsonPropertyName("exact")] bool Exact);
#endregion

#region Router POST /proxy
public sealed record ModuleProxyRequest(
    [property: JsonPropertyName("sourceModule")] string SourceModule,
    [property: JsonPropertyName("targetModule")] string TargetModule,
    [property: JsonPropertyName("targetPath")] string TargetPath, // Endpoint name like /chat, /generate
    [property: JsonPropertyName("method")] string Method, // GET, POST
    [property: JsonPropertyName("body")] JsonElement? Body);
#endregion

#region Intent catalog (intent_extractor POST /intent)
public sealed record IntentRequest(
    [property: JsonPropertyName("playerInput")] string PlayerInput);

public sealed record IntentResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("intent")] string Intent,
    [property: JsonPropertyName("params")] IReadOnlyDictionary<string, string> Parameters);
#endregion

#region LLM provider (llm_provider_qwen POST /generate and POST /chat)
/// <summary>POST /generate on an LLM provider: prompt and optional system text only; the provider picks the backing model from its own configuration.</summary>
public sealed record LlmGenerateRequest(
    [property: JsonPropertyName("prompt")] string Prompt,
    [property: JsonPropertyName("system")] string System = "You are a helpful assistant.");

/// <summary>
/// JSON envelope returned by an LLM provider module (e.g. llm_provider_qwen) on successful /generate.
/// </summary>
public sealed record LlmProviderGenerateResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("response")] string? Response,
    [property: JsonPropertyName("rawResponse")] string? RawResponse);

/// <summary>Request to llm_provider_qwen POST /chat: message list only; Ollama model comes from engine_config.json on the provider module.</summary>
public sealed record ChatGenerateRequest
{
    /// <summary>One chat message (Ollama /api/chat message shape).</summary>
    public sealed record ChatMessageDto(
        [property: JsonPropertyName("role")] string Role,
        [property: JsonPropertyName("content")] string Content);

    [property: JsonPropertyName("messages")]
    public IReadOnlyList<ChatMessageDto> Messages { get; init; } = Array.Empty<ChatMessageDto>();
}

/// <summary>JSON envelope returned by llm_provider_qwen on successful /chat (<see cref="Response"/> is assistant text).</summary>
public sealed record ChatGenerateResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("response")] string? Response,
    [property: JsonPropertyName("rawResponse")] string? RawResponse);
#endregion

#region Contract examples (engine_config template_contracts_id tooling)
public static class EngineContractExamples
{
    public sealed record EndpointTemplatePair(
        string? RequestBodyTemplate,
        string? ResponseBodyTemplate);

    private static readonly JsonSerializerOptions TemplateOptions = new()
    {
        WriteIndented = true
    };

    public static EndpointTemplatePair? TryGetTemplates(string? templateContractsId) => templateContractsId switch
    {
        "turn" => new EndpointTemplatePair(
            Serialize(new TurnRequest(1, "look around")),
            Serialize(new TurnResponse(true, "You stand still and listen."))),
        "initialize" => new EndpointTemplatePair(
            Serialize(new InitializeModuleRequest("sandcrawler", "00000000-0000-0000-0000-000000000001")),
            Serialize(new InitializeModuleResponse(true))),
        "persist_turn" => new EndpointTemplatePair(
            Serialize(new TurnPersistRequest(
                1,
                "look around",
                "{\"ok\":true,\"text\":\"You stand still and listen.\"}")),
            Serialize(new TurnPersistResponse(true))),
        "generate" => new EndpointTemplatePair(
            Serialize(new LlmGenerateRequest("Write a short response.")),
            Serialize(new LlmProviderGenerateResponse(true, "Short response.", "{\"raw\":\"...\"}"))),
        "intent" => new EndpointTemplatePair(
            Serialize(new IntentRequest("look around")),
            Serialize(new IntentResponse(true, "look_around", new Dictionary<string, string> { ["direction"] = "around" }))),
        "director_message" => new EndpointTemplatePair(
            Serialize(new DirectorMessageRequest(1, "Look around.")),
            Serialize(new DirectorMessageResponse(true, "You stand still and listen."))),
        "chat" => new EndpointTemplatePair(
            Serialize(new ChatGenerateRequest
            {
                Messages =
                [
                    new ChatGenerateRequest.ChatMessageDto("system", "You are the GM."),
                    new ChatGenerateRequest.ChatMessageDto("user", "Look around.")
                ]
            }),
            Serialize(new ChatGenerateResponse(true, "You stand still and listen.", "{\"raw\":\"...\"}"))),
        "memory_load_context" => new EndpointTemplatePair(
            Serialize(new MemoryLoadContextRequest(1, 12)),
            Serialize(new MemoryLoadContextResponse(
                true,
                [ExampleMemoryBlock()],
                [ExampleAgentMessage()],
                ExampleSnapshot(),
                ExampleMemoryBudget()))),
        "memory_persist_step" => new EndpointTemplatePair(
            Serialize(new MemoryPersistStepRequest(
                1,
                1,
                [ExampleAgentMessage()],
                [ExampleMemoryMutation()],
                [ExampleMemoryBlock()])),
            Serialize(new MemoryPersistStepResponse(true))),
        "memory_recall_search" => new EndpointTemplatePair(
            Serialize(new MemoryRecallSearchRequest("recent decisions", ["assistant"], 5)),
            Serialize(new MemoryRecallSearchResponse(true, [ExampleMemorySearchResult("recall")]))),
        "memory_archival_search" => new EndpointTemplatePair(
            Serialize(new MemoryArchivalSearchRequest("ancient ruins", ["lore"], 5)),
            Serialize(new MemoryArchivalSearchResponse(true, [ExampleMemorySearchResult("archival")]))),
        "memory_blocks_get_all" => new EndpointTemplatePair(
            Serialize(new MemoryBlocksGetAllRequest(true)),
            Serialize(new MemoryBlocksGetAllResponse(true, [ExampleMemoryBlock()]))),
        "memory_blocks_upsert" => new EndpointTemplatePair(
            Serialize(new MemoryBlockUpsertRequest(ExampleMemoryBlock())),
            Serialize(new MemoryBlockUpsertResponse(true))),
        "memory_messages_recent" => new EndpointTemplatePair(
            Serialize(new MemoryMessagesRecentRequest(12, ["assistant", "tool"])),
            Serialize(new MemoryMessagesRecentResponse(true, [ExampleAgentMessage()]))),
        "memory_messages_append" => new EndpointTemplatePair(
            Serialize(new MemoryMessageAppendRequest(ExampleAgentMessage())),
            Serialize(new MemoryMessageAppendResponse(true))),
        "memory_mutations_append" => new EndpointTemplatePair(
            Serialize(new MemoryMutationAppendRequest(ExampleMemoryMutation())),
            Serialize(new MemoryMutationAppendResponse(true))),
        "memory_snapshot_latest" => new EndpointTemplatePair(
            Serialize(new MemorySnapshotLatestRequest(true)),
            Serialize(new MemorySnapshotLatestResponse(true, ExampleSnapshot()))),
        "embed" => new EndpointTemplatePair(
            Serialize(new EmbeddingRequest("nomic-embed-text", ["The party enters the ruin."])),
            Serialize(new EmbeddingResponse(true, "nomic-embed-text", 3, [new EmbeddingVectorDto(0, [0.12f, -0.04f, 0.88f])]))),
        "token_count" => new EndpointTemplatePair(
            Serialize(new TokenCountRequest("qwen2.5:7b-instruct", "The party enters the ruin.")),
            Serialize(new TokenCountResponse(true, "qwen2.5:7b-instruct", 7, false))),
        "module_proxy" => new EndpointTemplatePair(
            Serialize(new ModuleProxyRequest(
                "intent_extractor",
                "generic_llm_provider",
                "/generate",
                "POST",
                JsonSerializer.SerializeToElement(new LlmGenerateRequest("Write a short response.")))),
            Serialize(new LlmProviderGenerateResponse(true, "Short response.", "{\"raw\":\"...\"}"))),
        "module_info" => new EndpointTemplatePair(
            null,
            Serialize(new ModuleInfoResponse(true, "router"))),
        "module_health" => new EndpointTemplatePair(
            null,
            Serialize(new ModuleHealthResponse(true, "ok", true))),
        _ => null
    };

    private static string Serialize<T>(T payload) =>
        JsonSerializer.Serialize(payload, TemplateOptions);

    private static MemoryBlockDto ExampleMemoryBlock() =>
        new("human", "Stable player-facing facts.", "Player prefers concise descriptions.", 2000, false);

    private static AgentMessageDto ExampleAgentMessage() =>
        new(1, 1, "assistant", "send_message", "You stand still and listen.");

    private static MemoryMutationDto ExampleMemoryMutation() =>
        new(1, 1, "core_memory_append", "human", null, "{\"append\":\"Player prefers concise descriptions.\"}");

    private static LatestSnapshotDto ExampleSnapshot() =>
        new(1, "{\"location\":\"dune\"}", "{\"visibleExits\":[\"north\"]}");

    private static MemoryBudgetDto ExampleMemoryBudget() =>
        new(4096, 2867, 12, 4000);

    private static MemorySearchResultDto ExampleMemorySearchResult(string source) =>
        new("example-1", "The party entered the ruin.", source, 0.92, "{\"turn\":1}");
}
#endregion
