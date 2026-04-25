using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

// Note: C# members use PascalCase; JSON wire names on these contracts use camelCase (JsonPropertyName). "params" stays as one word (not snake_case).

#region Module lifecycle (GET /info, /health; POST /shutdown)
public sealed record ModuleInfoResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("moduleName")] string ModuleName);

/// <summary>GET /health JSON. <see cref="Initialized"/> is false while awaiting or processing POST /initialize; true only when module-specific init is complete.</summary>
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

#region Contract examples (engine_config request_contract tooling)
public static class EngineContractExamples
{
    private static readonly JsonSerializerOptions TemplateOptions = new()
    {
        WriteIndented = true
    };

    public static string? TryGetRequestBodyTemplate(string? requestContract) => requestContract switch
    {
        "turn_request" => Serialize(new TurnRequest(
            1,
            "look around")),
        "initialize_request" => Serialize(new InitializeModuleRequest("sandcrawler", "00000000-0000-0000-0000-000000000001")),
        "session_turn_persist_request" => Serialize(new TurnPersistRequest(
            1,
            "look around",
            "{\"ok\":true,\"text\":\"You stand still and listen.\"}")),
        "qwen_generate_request" => Serialize(new LlmGenerateRequest("Write a short response.")),
        "intent_request" => Serialize(new IntentRequest("look around")),
        "director_message_request" => Serialize(new DirectorMessageRequest(1, "Look around.")),
        "chat_generate_request" => Serialize(new ChatGenerateRequest
        {
            Messages =
            [
                new ChatGenerateRequest.ChatMessageDto("system", "You are the GM."),
                new ChatGenerateRequest.ChatMessageDto("user", "Look around.")
            ]
        }),
        "module_proxy_request" => Serialize(new ModuleProxyRequest(
            "intent_extractor",
            "generic_llm_provider",
            "/generate",
            "POST",
            JsonSerializer.SerializeToElement(new LlmGenerateRequest("Write a short response.")))),
        _ => null
    };

    private static string Serialize<T>(T payload) =>
        JsonSerializer.Serialize(payload, TemplateOptions);
}
#endregion
