using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

public sealed record TurnRequest(
    [property: JsonPropertyName("playerInput")] string PlayerInput);

public sealed record LlmGenerateRequest(
    [property: JsonPropertyName("prompt")] string Prompt,
    [property: JsonPropertyName("model")] string Model,
    [property: JsonPropertyName("system")] string System = "You are a helpful assistant.");

public sealed record IntentRequest(
    [property: JsonPropertyName("playerInput")] string PlayerInput);

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
        "turn_request" => Serialize(new TurnRequest("look around")),
        "qwen_generate_request" => Serialize(new LlmGenerateRequest("Write a short response.", "qwen2.5:7b-instruct")),
        "intent_request" => Serialize(new IntentRequest("look around")),
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
