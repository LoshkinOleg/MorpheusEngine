using System.Text.Json;
using MorpheusEngine;

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

var configuration = EngineConfigLoader.GetConfiguration();
var genericDirector = configuration.GetRequiredGenericDirectorModule();
Require(genericDirector.PortKey == "memory_director", "generic_director should resolve to memory_director for Phase 1.");
var memoryDirectorOptions = genericDirector.MemoryDirectorOptions
    ?? throw new InvalidOperationException("memory_director options should be parsed.");
Require(memoryDirectorOptions.MaxStepsPerTurn == 12, "Unexpected max_steps_per_turn.");

var schemaPath = Path.Combine(configuration.RepositoryRoot, "docs", "schemas", "memory_director_action.schema.json");
using var schemaDoc = JsonDocument.Parse(File.ReadAllText(schemaPath));
var schema = schemaDoc.RootElement;
Require(schema.GetProperty("additionalProperties").GetBoolean() == false, "Action schema should reject extra top-level properties.");
var tools = schema.GetProperty("properties").GetProperty("tool").GetProperty("enum")
    .EnumerateArray()
    .Select(static item => item.GetString())
    .ToHashSet(StringComparer.Ordinal);
foreach (var tool in new[] { "send_message", "core_memory_append", "core_memory_replace", "core_memory_set", "get_current_snapshot" })
{
    Require(tools.Contains(tool), "Action schema is missing tool " + tool + ".");
}

var chat = new ChatGenerateRequest
{
    Messages = [new ChatGenerateRequest.ChatMessageDto("user", "look around")],
    Format = schema.Clone(),
    KeepAlive = "30m"
};
var serialized = JsonSerializer.Serialize(chat);
Require(serialized.Contains("\"format\""), "ChatGenerateRequest should serialize format.");
Require(serialized.Contains("\"keepAlive\":\"30m\""), "ChatGenerateRequest should serialize keepAlive.");

Console.WriteLine("Phase 1 focused smoke tests passed.");
