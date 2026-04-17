using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

/// <summary>Player-facing turn envelope: identifies the run and turn index before the router forwards <see cref="IntentRequest"/> downstream.</summary>
public sealed record TurnRequest(
    [property: JsonPropertyName("runId")] string RunId,
    [property: JsonPropertyName("gameProjectId")] string GameProjectId,
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerId")] string PlayerId,
    [property: JsonPropertyName("playerInput")] string PlayerInput);

public sealed record RunStartRequest(
    [property: JsonPropertyName("gameProjectId")] string GameProjectId,
    [property: JsonPropertyName("runId")] string RunId);

public sealed record RunStartResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("runId")] string RunId,
    [property: JsonPropertyName("gameProjectId")] string GameProjectId);

public sealed record TurnValidateRequest(
    [property: JsonPropertyName("gameProjectId")] string GameProjectId,
    [property: JsonPropertyName("runId")] string RunId,
    [property: JsonPropertyName("turn")] int Turn);

public sealed record TurnValidateResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("expectedTurn")] int ExpectedTurn,
    [property: JsonPropertyName("maxSnapshotTurn")] int MaxSnapshotTurn,
    [property: JsonPropertyName("error")] string? Error = null);

public sealed record TurnPersistRequest(
    [property: JsonPropertyName("gameProjectId")] string GameProjectId,
    [property: JsonPropertyName("runId")] string RunId,
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerId")] string PlayerId,
    [property: JsonPropertyName("playerInput")] string PlayerInput,
    [property: JsonPropertyName("intentResponseBody")] string IntentResponseBody);

public sealed record TurnPersistResponse(
    [property: JsonPropertyName("ok")] bool Ok);

public sealed record LlmGenerateRequest(
    [property: JsonPropertyName("prompt")] string Prompt,
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("system")] string System = "You are a helpful assistant.");

public sealed record IntentRequest(
    [property: JsonPropertyName("playerInput")] string PlayerInput);

/// <summary>Router forwards <see cref="TurnRequest"/>-shaped JSON to <c>director</c> <c>POST /message</c> (same field names as <see cref="TurnRequest"/>).</summary>
public sealed record DirectorMessageRequest(
    [property: JsonPropertyName("runId")] string RunId,
    [property: JsonPropertyName("gameProjectId")] string GameProjectId,
    [property: JsonPropertyName("turn")] int Turn,
    [property: JsonPropertyName("playerId")] string PlayerId,
    [property: JsonPropertyName("playerInput")] string PlayerInput);

/// <summary>One chat message for <c>llm_provider_qwen</c> <c>POST /chat</c> (Ollama <c>/api/chat</c> message shape).</summary>
public sealed record ChatMessageDto(
    [property: JsonPropertyName("role")] string Role,
    [property: JsonPropertyName("content")] string Content);

/// <summary>Request to <c>llm_provider_qwen</c> <c>POST /chat</c>: full message list (system + history + latest user).</summary>
public sealed record ChatGenerateRequest(
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("messages")] IReadOnlyList<ChatMessageDto> Messages);

/// <summary>JSON envelope returned by <c>llm_provider_qwen</c> on successful <c>/chat</c> (<see cref="Response"/> is assistant text).</summary>
public sealed record ChatGenerateResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("model")] string? Model,
    [property: JsonPropertyName("response")] string? Response,
    [property: JsonPropertyName("raw_response")] string? RawResponse);

public sealed record ModuleProxyRequest(
    [property: JsonPropertyName("source_module")] string SourceModule,
    [property: JsonPropertyName("target_module")] string TargetModule,
    [property: JsonPropertyName("target_path")] string TargetPath,
    [property: JsonPropertyName("method")] string Method,
    [property: JsonPropertyName("body")] JsonElement? Body);

public sealed record ModuleInfoResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("module_name")] string ModuleName);

public sealed record ModuleHealthResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("module_name")] string ModuleName,
    [property: JsonPropertyName("status")] string Status);

public sealed record ModuleShutdownResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("module_name")] string ModuleName,
    [property: JsonPropertyName("message")] string Message);

public sealed record ErrorResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("error")] string Error,
    [property: JsonPropertyName("details")] string? Details = null);

/// <summary>
/// JSON envelope returned by an LLM provider module (e.g. <c>llm_provider_qwen</c>) on successful <c>/generate</c>.
/// </summary>
public sealed record LlmProviderGenerateResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("model")] string? Model,
    [property: JsonPropertyName("response")] string? Response,
    [property: JsonPropertyName("raw_response")] string? RawResponse);

public sealed record IntentResponse(
    [property: JsonPropertyName("ok")] bool Ok,
    [property: JsonPropertyName("intent")] string Intent,
    [property: JsonPropertyName("params")] IReadOnlyDictionary<string, string> Parameters);

public static class EngineContractExamples
{
    private static readonly JsonSerializerOptions TemplateOptions = new()
    {
        WriteIndented = true
    };

    public static string? TryGetRequestBodyTemplate(string? requestContract) => requestContract switch
    {
        "turn_request" => Serialize(new TurnRequest(
            "00000000-0000-0000-0000-000000000001",
            "default",
            1,
            "player",
            "look around")),
        "initialize_request" => Serialize(new RunStartRequest("default", "00000000-0000-0000-0000-000000000001")),
        "session_turn_validate_request" => Serialize(new TurnValidateRequest("default", "00000000-0000-0000-0000-000000000001", 1)),
        "session_turn_persist_request" => Serialize(new TurnPersistRequest(
            "default",
            "00000000-0000-0000-0000-000000000001",
            1,
            "player",
            "look around",
            "{\"ok\":true,\"intent\":\"wait\",\"params\":{}}")),
        "qwen_generate_request" => Serialize(new LlmGenerateRequest("Write a short response.", "qwen2.5:7b-instruct")),
        "intent_request" => Serialize(new IntentRequest("look around")),
        "director_message_request" => Serialize(new DirectorMessageRequest(
            "00000000-0000-0000-0000-000000000001",
            "sandcrawler",
            1,
            "player",
            "Look around.")),
        "chat_generate_request" => Serialize(new ChatGenerateRequest(
            "qwen2.5:7b-instruct",
            new ChatMessageDto[]
            {
                new("system", "You are the GM."),
                new("user", "Look around.")
            })),
        "module_proxy_request" => Serialize(new ModuleProxyRequest(
            "intent_extractor",
            "generic_llm_provider",
            "/generate",
            "POST",
            JsonSerializer.SerializeToElement(new LlmGenerateRequest("Write a short response.", "qwen2.5:7b-instruct")))),
        _ => null
    };

    private static string Serialize<T>(T payload) =>
        JsonSerializer.Serialize(payload, TemplateOptions);
}
