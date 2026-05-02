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
Require(genericDirector.PortKey == "memory_director", "generic_director should resolve to memory_director for the default sandcrawler pipeline.");
var memoryDirectorOptions = genericDirector.MemoryDirectorOptions
    ?? throw new InvalidOperationException("memory_director options should be parsed.");
Require(memoryDirectorOptions.MaxStepsPerTurn == 12, "Unexpected max_steps_per_turn.");
var sandcrawlerManifest = GameProjectManifestLoader.Load(configuration.RepositoryRoot, "sandcrawler");
Require(sandcrawlerManifest.TurnPipeline == "memory_director_default", "sandcrawler should select the memory_director_default turn pipeline.");
var memoryPipeline = configuration.GetRequiredTurnPipeline(sandcrawlerManifest.TurnPipeline);
Require(memoryPipeline.Steps.Count == 2, "memory_director_default should preserve the two-step director+persistence flow.");
Require(memoryPipeline.Steps[0].Id == "director_message", "memory_director_default first step should call the director.");
Require(memoryPipeline.Steps[0].TargetModule == "generic_director", "memory_director_default should call generic_director.");
Require(memoryPipeline.Steps[0].Path == "/message" && memoryPipeline.Steps[0].Method == "POST", "memory_director_default director step should target POST /message.");
Require(memoryPipeline.Steps[1].TargetModule == "session_store", "memory_director_default should persist through session_store.");
Require(memoryPipeline.Steps[1].Path == "/persist_turn" && memoryPipeline.Steps[1].Method == "POST", "memory_director_default persistence step should target POST /persist_turn.");
Require(memoryPipeline.ResponseMapping.SourceStep == "director_message", "memory_director_default should map TurnResponse from the director step.");
var simplePipeline = configuration.GetRequiredTurnPipeline("simple_director_default");
Require(simplePipeline.Steps.Count == 2, "simple_director_default should preserve the two-step simple director flow.");
foreach (var pipeline in configuration.TurnPipelines.Values)
{
    foreach (var step in pipeline.Steps)
    {
        var module = configuration.FindModule(configuration.ResolveProxyTargetModuleKey(step.TargetModule))
            ?? throw new InvalidOperationException("Pipeline step target should resolve to a configured module.");
        Require(
            module.Endpoints.Any(endpoint => endpoint.Path == step.Path && endpoint.Method == step.Method),
            $"Pipeline '{pipeline.Id}' step '{step.Id}' should target an allowlisted endpoint.");
    }
}
var genericEmbeddings = configuration.GetRequiredGenericEmbeddingsModule();
Require(genericEmbeddings.PortKey == "embeddings_ollama", "generic_embeddings should resolve to embeddings_ollama for Phase 3.");
var embeddingsOptions = genericEmbeddings.EmbeddingsOptions
    ?? throw new InvalidOperationException("embeddings_ollama options should be parsed.");
Require(embeddingsOptions.DefaultEmbeddingModel == "nomic-embed-text", "Unexpected default embedding model.");
Require(genericEmbeddings.Endpoints.Any(static endpoint => endpoint.Path == "/embed" && endpoint.Method == "POST"), "Embeddings module should expose POST /embed.");
var genericProvider = configuration.GetRequiredGenericLlmProviderModule();
Require(genericProvider.Endpoints.Any(static endpoint => endpoint.Path == "/token_count" && endpoint.Method == "POST"), "Generic LLM provider should expose POST /token_count.");

var schemaPath = Path.Combine(configuration.RepositoryRoot, "docs", "schemas", "memory_director_action.schema.json");
using var schemaDoc = JsonDocument.Parse(File.ReadAllText(schemaPath));
var schema = schemaDoc.RootElement;
Require(schema.GetProperty("additionalProperties").GetBoolean() == false, "Action schema should reject extra top-level properties.");
var tools = schema.GetProperty("properties").GetProperty("tool").GetProperty("enum")
    .EnumerateArray()
    .Select(static item => item.GetString())
    .ToHashSet(StringComparer.Ordinal);
foreach (var tool in new[] { "send_message", "core_memory_append", "core_memory_replace", "core_memory_set", "get_current_snapshot", "recall_search", "archival_memory_insert", "archival_memory_search" })
{
    Require(tools.Contains(tool), "Action schema is missing tool " + tool + ".");
}

var recallSearchBranch = schema.GetProperty("oneOf")
    .EnumerateArray()
    .FirstOrDefault(branch =>
        branch.GetProperty("properties").GetProperty("tool").TryGetProperty("const", out var toolConst)
        && toolConst.GetString() == "recall_search");
Require(recallSearchBranch.ValueKind == JsonValueKind.Object, "Action schema should include recall_search branch.");
var recallSearchProperties = recallSearchBranch.GetProperty("properties").GetProperty("arguments").GetProperty("properties");
Require(recallSearchProperties.TryGetProperty("query", out _), "recall_search should require a query argument.");
Require(recallSearchProperties.TryGetProperty("roles", out _), "recall_search should allow role filters.");
Require(recallSearchProperties.TryGetProperty("limit", out _), "recall_search should allow a result limit.");
var archivalSearchBranch = schema.GetProperty("oneOf")
    .EnumerateArray()
    .FirstOrDefault(branch =>
        branch.GetProperty("properties").GetProperty("tool").TryGetProperty("const", out var toolConst)
        && toolConst.GetString() == "archival_memory_search");
Require(archivalSearchBranch.ValueKind == JsonValueKind.Object, "Action schema should include archival_memory_search branch.");
var archivalSearchProperties = archivalSearchBranch.GetProperty("properties").GetProperty("arguments").GetProperty("properties");
Require(archivalSearchProperties.TryGetProperty("query", out _), "archival_memory_search should require a query argument.");
Require(archivalSearchProperties.TryGetProperty("tags", out _), "archival_memory_search should allow tag filters.");
Require(archivalSearchProperties.TryGetProperty("topK", out _), "archival_memory_search should allow a result limit.");
var archivalInsertBranch = schema.GetProperty("oneOf")
    .EnumerateArray()
    .FirstOrDefault(branch =>
        branch.GetProperty("properties").GetProperty("tool").TryGetProperty("const", out var toolConst)
        && toolConst.GetString() == "archival_memory_insert");
Require(archivalInsertBranch.ValueKind == JsonValueKind.Object, "Action schema should include archival_memory_insert branch.");
var archivalInsertRequired = archivalInsertBranch.GetProperty("properties").GetProperty("arguments").GetProperty("required")
    .EnumerateArray()
    .Select(static item => item.GetString())
    .ToHashSet(StringComparer.Ordinal);
Require(archivalInsertRequired.Contains("content"), "archival_memory_insert should require content.");

var chat = new ChatGenerateRequest
{
    Messages = [new ChatGenerateRequest.ChatMessageDto("user", "look around")],
    Format = schema.Clone(),
    KeepAlive = "30m"
};
var serialized = JsonSerializer.Serialize(chat);
Require(serialized.Contains("\"format\""), "ChatGenerateRequest should serialize format.");
Require(serialized.Contains("\"keepAlive\":\"30m\""), "ChatGenerateRequest should serialize keepAlive.");

Require(EngineContractExamples.TryGetTemplates("memory_summaries_recent") is not null, "memory_summaries_recent example should exist.");
Require(EngineContractExamples.TryGetTemplates("memory_recall_compact") is not null, "memory_recall_compact example should exist.");
Require(EngineContractExamples.TryGetTemplates("memory_archival_upsert") is not null, "memory_archival_upsert example should exist.");
Require(EngineContractExamples.TryGetTemplates("token_count") is not null, "token_count example should exist.");
Require(EngineContractExamples.TryGetTemplates("memory_pipeline_events_recent") is not null, "memory_pipeline_events_recent example should exist.");
var loadContext = new MemoryLoadContextResponse(
    true,
    [],
    [],
    new LatestSnapshotDto(0, "{}", "{}"),
    new MemoryBudgetDto(4096, 2867, 12, 4000),
    [new MemorySummaryDto(1, 3, "Earlier events summarized.", 9)],
    new MemoryContextAccountingDto(100, 200, ["none"]));
var loadContextJson = JsonSerializer.Serialize(loadContext);
Require(loadContextJson.Contains("\"summaries\""), "MemoryLoadContextResponse should serialize summaries.");
Require(loadContextJson.Contains("\"accounting\""), "MemoryLoadContextResponse should serialize accounting.");
var accounting = new MemoryContextAccountingDto(
    480,
    1200,
    ["message:1:1:player"],
    4096,
    2867,
    120,
    false,
    [
        new MemoryContextItemDto("agent_prompt", "system", "included", 100, 100),
        new MemoryContextItemDto("message:1:1:player", "recent_message", "omitted", 500, 0, "context_budget")
    ]);
var persistWithTelemetry = new MemoryPersistStepRequest(1, 1, [], [], [], accounting);
var persistWithTelemetryJson = JsonSerializer.Serialize(persistWithTelemetry);
Require(persistWithTelemetryJson.Contains("\"contextAccounting\""), "MemoryPersistStepRequest should serialize optional contextAccounting.");
Require(persistWithTelemetryJson.Contains("\"targetTokens\":2867"), "Context accounting should serialize token target.");
Require(persistWithTelemetryJson.Contains("\"status\":\"omitted\""), "Context accounting should serialize per-item statuses.");
var pipelineEvents = new MemoryPipelineEventsRecentResponse(
    true,
    [new MemoryPipelineEventDto(1, 1, JsonSerializer.Serialize(new { eventType = "memory_context_budget", accounting }), "2026-05-02T08:00:00Z")]);
Require(JsonSerializer.Serialize(pipelineEvents).Contains("memory_context_budget"), "Pipeline event response should expose budget telemetry payloads.");

var embeddingResponse = new EmbeddingResponse(
    true,
    "nomic-embed-text",
    2,
    [new EmbeddingVectorDto(0, [0.1f, 0.2f]), new EmbeddingVectorDto(1, [0.3f, 0.4f])]);
Require(embeddingResponse.Vectors.Select(static vector => vector.Index).SequenceEqual([0, 1]), "Embedding vector order should preserve input order.");
var archivalSearch = new MemoryArchivalSearchRequest("sealed north door", ["lore"], 3, [0.1f, 0.2f], "nomic-embed-text");
var archivalSearchJson = JsonSerializer.Serialize(archivalSearch);
Require(archivalSearchJson.Contains("\"queryEmbedding\""), "MemoryArchivalSearchRequest should serialize queryEmbedding.");
Require(archivalSearchJson.Contains("\"embeddingModel\":\"nomic-embed-text\""), "MemoryArchivalSearchRequest should serialize embeddingModel.");
var passage = new ArchivalPassageDto(
    "agent:test",
    "run",
    "test",
    "The north door is sealed.",
    ["fact"],
    "{\"turn\":1}",
    "nomic-embed-text",
    2,
    [0.1f, 0.2f]);
var upsertJson = JsonSerializer.Serialize(new MemoryArchivalUpsertRequest(passage));
Require(upsertJson.Contains("\"embeddingDimensions\":2"), "Archival passage should serialize embedding dimensions.");
Require(upsertJson.Contains("\"embedding\""), "Archival passage should serialize embedding vector.");

Console.WriteLine("Phase 1/2/3/4/5 focused smoke tests passed.");
