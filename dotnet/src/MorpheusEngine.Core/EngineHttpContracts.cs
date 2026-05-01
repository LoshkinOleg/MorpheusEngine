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
}
#endregion
