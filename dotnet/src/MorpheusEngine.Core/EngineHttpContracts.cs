using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

public sealed record TurnRequest(
    [property: JsonPropertyName("playerInput")] string PlayerInput);

public sealed record QwenGenerateRequest(
    [property: JsonPropertyName("prompt")] string Prompt,
    [property: JsonPropertyName("model")] string Model = "qwen2.5:7b-instruct",
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
        "qwen_generate_request" => Serialize(new QwenGenerateRequest("Write a short response.")),
        "intent_request" => Serialize(new IntentRequest("look around")),
        "module_proxy_request" => Serialize(new ModuleProxyRequest(
            "intent_extractor",
            "llm_provider_qwen",
            "/generate",
            "POST",
            JsonSerializer.SerializeToElement(new QwenGenerateRequest("Write a short response.")))),
        _ => null
    };

    private static string Serialize<T>(T payload) =>
        JsonSerializer.Serialize(payload, TemplateOptions);
}
