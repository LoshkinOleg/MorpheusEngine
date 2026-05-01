using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// Memory-managed Director replacement: compiles context from session_store, asks the LLM for one constrained action at a time, executes Phase 1 tools, and returns one narration.
/// </summary>
public sealed class MemoryDirector
{
    #region Nested types
    private sealed record AgentAction(string Thought, string Tool, JsonElement Arguments);

    private sealed record ToolExecutionResult(
        bool Ok,
        string ToolResultContent,
        IReadOnlyList<MemoryBlockDto> BlockUpdates,
        IReadOnlyList<MemoryMutationDto> Mutations,
        string? FinalMessage);
    #endregion

    #region Private data
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
    private readonly HttpClient _httpClient = new()
    {
        Timeout = TimeSpan.FromSeconds(60)
    };
    private readonly RouterProxyClient _routerProxy;
    private readonly HttpListener _listener = new();
    private readonly SemaphoreSlim _sessionGate = new(1, 1);
    private readonly JsonElement _actionSchema;

    private bool _shutdownRequested = false;
    private volatile bool _initialized = false;
    private volatile bool _initializing = false;
    private string _boundGameProjectId = string.Empty;
    private string _boundRunId = string.Empty;
    private string _agentPrompt = string.Empty;
    private MemoryDirectorModuleOptions _options = new(12, 4000, 12, "30m");
    #endregion

    #region Public methods
    public MemoryDirector()
    {
        _routerProxy = new RouterProxyClient(_httpClient, _configuration, "memory_director", JsonOptions);
        _actionSchema = LoadActionSchema();
    }

    public async Task RunAsync()
    {
        Initialize();

        try
        {
            while (!_shutdownRequested)
            {
                var context = await _listener.GetContextAsync();
                _ = ProcessQueryAsync(context);
            }
        }
        catch (HttpListenerException e)
        {
            Console.WriteLine("MemoryDirector error encountered: " + e.Message);
        }
        finally
        {
            Shutdown();
        }
    }

    public void RequestShutdown() => _shutdownRequested = true;
    #endregion

    #region Private methods
    private void Initialize()
    {
        var module = _configuration.FindModule("memory_director")
            ?? throw new InvalidOperationException("engine_config.json must include module 'memory_director'.");
        if (module.MemoryDirectorOptions is null)
        {
            throw new InvalidOperationException("memory_director module options are required.");
        }

        _options = module.MemoryDirectorOptions;
        var port = _configuration.GetRequiredListenPort("memory_director");
        _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        _listener.Start();
        Console.WriteLine($"ready listen=http://127.0.0.1:{port}/");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
        _httpClient.Dispose();
        _sessionGate.Dispose();
        Console.WriteLine("MemoryDirector shut down.");
    }

    private async Task ProcessQueryAsync(HttpListenerContext context)
    {
        try
        {
            if (context.Request.Url is null)
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid request URL."));
                return;
            }

            var path = context.Request.Url.AbsolutePath;
            var method = context.Request.HttpMethod.Trim().ToUpperInvariant();

            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                await RespondJsonAsync(context, 200, new ModuleInfoResponse(true, "memory_director"));
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                if (_initializing)
                {
                    await RespondJsonAsync(context, 503, new ModuleHealthResponse(false, "initializing", false));
                    return;
                }

                await RespondJsonAsync(
                    context,
                    200,
                    _initialized
                        ? new ModuleHealthResponse(true, "healthy", true)
                        : new ModuleHealthResponse(false, "awaiting_initialize", false));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await RespondJsonAsync(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
                _shutdownRequested = true;
                try
                {
                    _listener.Stop();
                }
                catch (ObjectDisposedException)
                {
                }
                catch (HttpListenerException)
                {
                }

                return;
            }

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await ProcessRequest_bindRun(context);
                return;
            }

            if (path.Equals("/message", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await ProcessRequest_message(context);
                return;
            }

            await RespondJsonAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            Console.WriteLine("MemoryDirector encountered unhandled request error: " + e.Message);
            if (context.Response.OutputStream.CanWrite)
            {
                await RespondJsonAsync(context, 500, new ErrorResponse(false, "Unhandled memory_director error.", e.Message));
            }
        }
    }

    private async Task ProcessRequest_bindRun(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        InitializeModuleRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null || string.IsNullOrWhiteSpace(request.GameProjectId) || string.IsNullOrWhiteSpace(request.RunId))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty gameProjectId and runId."));
            return;
        }

        _initializing = true;
        try
        {
            await _sessionGate.WaitAsync();
            try
            {
                if (_initialized)
                {
                    await RespondJsonAsync(
                        context,
                        409,
                        new ErrorResponse(false, "MemoryDirector already bound for this process; restart it to bind another run."));
                    return;
                }

                _boundGameProjectId = request.GameProjectId.Trim();
                _boundRunId = request.RunId.Trim();
                _agentPrompt = LoadAgentPrompt(_boundGameProjectId);
                await SeedCoreMemoryIfEmptyAsync(_boundGameProjectId);
                _initialized = true;
                await RespondJsonAsync(context, 200, new InitializeModuleResponse(true));
            }
            finally
            {
                _sessionGate.Release();
            }
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to initialize memory_director.", e.Message));
        }
        finally
        {
            _initializing = false;
        }
    }

    private async Task ProcessRequest_message(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        DirectorMessageRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<DirectorMessageRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty playerInput."));
            return;
        }

        if (request.Turn < 1)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Turn must be >= 1."));
            return;
        }

        await _sessionGate.WaitAsync();
        try
        {
            if (!_initialized)
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, "MemoryDirector run is not bound; call /initialize first."));
                return;
            }

            var playerInput = request.PlayerInput.Trim();
            await PersistStepAsync(
                new MemoryPersistStepRequest(
                    request.Turn,
                    0,
                    [new AgentMessageDto(request.Turn, 0, "player", "player_input", playerInput)],
                    [],
                    []));

            var failureCounts = new Dictionary<string, int>(StringComparer.Ordinal);
            string? lastThought = null;

            for (var step = 1; step <= _options.MaxStepsPerTurn; step++)
            {
                var memoryContext = await LoadContextAsync(request.Turn);
                var chatMessages = CompileContext(memoryContext);
                var llmResponse = await ChatAsync(chatMessages);
                if (llmResponse.Payload is null || !llmResponse.Payload.Ok || string.IsNullOrWhiteSpace(llmResponse.Payload.Response))
                {
                    await RespondJsonAsync(context, 502, new ErrorResponse(false, "LLM chat failed or returned empty response.", llmResponse.RawBody));
                    return;
                }

                if (!TryParseAction(llmResponse.Payload.Response, out var action, out var parseError))
                {
                    var finalFromParse = await HandleRecoverableFailureAsync(
                        request.Turn,
                        step,
                        "schema_violation",
                        "parse:" + parseError,
                        parseError,
                        failureCounts,
                        lastThought);
                    if (finalFromParse is not null)
                    {
                        await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, finalFromParse));
                        return;
                    }

                    continue;
                }

                lastThought = action.Thought;
                var canonicalToolCall = action.Tool + ":" + CanonicalizeJson(action.Arguments);
                var toolResult = ExecuteTool(request.Turn, step, memoryContext, action);

                var assistantMessage = new AgentMessageDto(
                    request.Turn,
                    step,
                    "assistant",
                    action.Tool == "send_message" ? "send_message" : "tool_call",
                    llmResponse.Payload.Response,
                    action.Tool);
                var toolMessages = new List<AgentMessageDto> { assistantMessage };
                if (action.Tool != "send_message")
                {
                    toolMessages.Add(new AgentMessageDto(
                        request.Turn,
                        step,
                        "tool",
                        toolResult.Ok ? "tool_result" : "tool_error",
                        TruncateToolResult(toolResult.ToolResultContent),
                        action.Tool));
                }

                await PersistStepAsync(
                    new MemoryPersistStepRequest(
                        request.Turn,
                        step,
                        toolMessages,
                        toolResult.Mutations,
                        toolResult.BlockUpdates));

                if (toolResult.FinalMessage is not null)
                {
                    await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, toolResult.FinalMessage));
                    return;
                }

                if (!toolResult.Ok)
                {
                    var finalFromTool = await HandleRecoverableFailureAsync(
                        request.Turn,
                        step,
                        action.Tool,
                        canonicalToolCall,
                        toolResult.ToolResultContent,
                        failureCounts,
                        lastThought,
                        alreadyPersisted: true);
                    if (finalFromTool is not null)
                    {
                        await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, finalFromTool));
                        return;
                    }
                }
            }

            var final = await SynthesizeAndPersistFinalAsync(request.Turn, _options.MaxStepsPerTurn + 1, lastThought);
            await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, final));
        }
        finally
        {
            _sessionGate.Release();
        }
    }

    private async Task<string?> HandleRecoverableFailureAsync(
        int turn,
        int step,
        string toolName,
        string canonicalFailureKey,
        string error,
        Dictionary<string, int> failureCounts,
        string? lastThought,
        bool alreadyPersisted = false)
    {
        failureCounts.TryGetValue(canonicalFailureKey, out var previousFailures);
        failureCounts[canonicalFailureKey] = previousFailures + 1;
        if (!alreadyPersisted)
        {
            await PersistStepAsync(
                new MemoryPersistStepRequest(
                    turn,
                    step,
                    [new AgentMessageDto(turn, step, "tool", "tool_error", TruncateToolResult(error), toolName)],
                    [],
                    []));
        }

        if (previousFailures > 0)
        {
            return await SynthesizeAndPersistFinalAsync(turn, step + 1, lastThought);
        }

        return null;
    }

    private async Task<string> SynthesizeAndPersistFinalAsync(int turn, int step, string? lastThought)
    {
        var final = string.IsNullOrWhiteSpace(lastThought)
            ? "The situation hangs unresolved for a moment; describe your next move."
            : "The scene settles for a moment. " + lastThought.Trim();
        await PersistStepAsync(
            new MemoryPersistStepRequest(
                turn,
                step,
                [new AgentMessageDto(turn, step, "assistant", "send_message", final, "send_message")],
                [],
                []));
        return final;
    }

    private ToolExecutionResult ExecuteTool(int turn, int step, MemoryLoadContextResponse memoryContext, AgentAction action)
    {
        try
        {
            return action.Tool switch
            {
                "send_message" => ExecuteSendMessage(action.Arguments),
                "core_memory_append" => ExecuteCoreMemoryAppend(turn, step, memoryContext, action.Arguments),
                "core_memory_replace" => ExecuteCoreMemoryReplace(turn, step, memoryContext, action.Arguments),
                "core_memory_set" => ExecuteCoreMemorySet(turn, step, memoryContext, action.Arguments),
                "get_current_snapshot" => ExecuteGetCurrentSnapshot(memoryContext),
                _ => new ToolExecutionResult(false, "Unknown tool: " + action.Tool, [], [], null)
            };
        }
        catch (Exception e)
        {
            return new ToolExecutionResult(false, e.Message, [], [], null);
        }
    }

    private static ToolExecutionResult ExecuteSendMessage(JsonElement arguments)
    {
        var message = RequireString(arguments, "message").Trim();
        return new ToolExecutionResult(true, JsonSerializer.Serialize(new { sent = true }), [], [], message);
    }

    private static ToolExecutionResult ExecuteCoreMemoryAppend(int turn, int step, MemoryLoadContextResponse memoryContext, JsonElement arguments)
    {
        var label = RequireString(arguments, "label").Trim();
        var content = RequireString(arguments, "content");
        var block = FindBlock(memoryContext, label);
        var newValue = string.IsNullOrEmpty(block.Value) ? content : block.Value + Environment.NewLine + content;
        return BuildBlockUpdateResult(turn, step, "core_memory_append", block, block with { Value = newValue });
    }

    private static ToolExecutionResult ExecuteCoreMemoryReplace(int turn, int step, MemoryLoadContextResponse memoryContext, JsonElement arguments)
    {
        var label = RequireString(arguments, "label").Trim();
        var oldValue = RequireString(arguments, "oldValue");
        var newValue = RequireString(arguments, "newValue");
        var block = FindBlock(memoryContext, label);
        var firstIndex = block.Value.IndexOf(oldValue, StringComparison.Ordinal);
        if (firstIndex < 0)
        {
            throw new InvalidOperationException($"Memory block '{label}' does not contain the requested oldValue.");
        }

        if (block.Value.IndexOf(oldValue, firstIndex + oldValue.Length, StringComparison.Ordinal) >= 0)
        {
            throw new InvalidOperationException($"Memory block '{label}' contains oldValue more than once; replacement is ambiguous.");
        }

        var updated = block.Value.Remove(firstIndex, oldValue.Length).Insert(firstIndex, newValue);
        return BuildBlockUpdateResult(turn, step, "core_memory_replace", block, block with { Value = updated });
    }

    private static ToolExecutionResult ExecuteCoreMemorySet(int turn, int step, MemoryLoadContextResponse memoryContext, JsonElement arguments)
    {
        var label = RequireString(arguments, "label").Trim();
        var value = RequireString(arguments, "value");
        var block = FindBlock(memoryContext, label);
        return BuildBlockUpdateResult(turn, step, "core_memory_set", block, block with { Value = value });
    }

    private static ToolExecutionResult ExecuteGetCurrentSnapshot(MemoryLoadContextResponse memoryContext)
    {
        return new ToolExecutionResult(true, JsonSerializer.Serialize(memoryContext.LatestSnapshot), [], [], null);
    }

    private static ToolExecutionResult BuildBlockUpdateResult(int turn, int step, string toolName, MemoryBlockDto before, MemoryBlockDto after)
    {
        if (before.ReadOnly)
        {
            throw new InvalidOperationException($"Memory block '{before.Label}' is read-only.");
        }

        if (after.Value.Length > after.CharLimit)
        {
            throw new InvalidOperationException(
                $"Memory block '{after.Label}' value length {after.Value.Length} exceeds charLimit {after.CharLimit}.");
        }

        var mutation = new MemoryMutationDto(
            turn,
            step,
            toolName,
            after.Label,
            JsonSerializer.Serialize(before),
            JsonSerializer.Serialize(after));
        return new ToolExecutionResult(true, JsonSerializer.Serialize(new { updated = after.Label }), [after], [mutation], null);
    }

    private IReadOnlyList<ChatGenerateRequest.ChatMessageDto> CompileContext(MemoryLoadContextResponse memoryContext)
    {
        var system = new StringBuilder();
        system.AppendLine(_agentPrompt);
        system.AppendLine();
        system.AppendLine("You must answer with exactly one JSON action matching the provided schema. Do not include markdown.");
        system.AppendLine("Tool rules: send_message is terminal. Memory-edit and snapshot tools are non-terminal.");
        system.AppendLine("Core memory blocks:");
        foreach (var block in memoryContext.Blocks.OrderBy(static block => block.Label, StringComparer.OrdinalIgnoreCase))
        {
            system.AppendLine($"[{block.Label}] {block.Description}");
            system.AppendLine(block.Value);
        }

        system.AppendLine("Latest snapshot:");
        system.AppendLine(memoryContext.LatestSnapshot.WorldStateJson);
        system.AppendLine(memoryContext.LatestSnapshot.ViewStateJson);

        var messages = new List<ChatGenerateRequest.ChatMessageDto>
        {
            new("system", system.ToString())
        };
        foreach (var message in memoryContext.RecentMessages)
        {
            messages.Add(new ChatGenerateRequest.ChatMessageDto(MapRoleForChat(message.Role), FormatMemoryMessage(message)));
        }

        return messages;
    }

    private async Task<RouterProxyResponse<ChatGenerateResponse>> ChatAsync(IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages)
    {
        return await _routerProxy.PostAsync<ChatGenerateRequest, ChatGenerateResponse>(
            "generic_llm_provider",
            "/chat",
            new ChatGenerateRequest
            {
                Messages = messages,
                Format = _actionSchema,
                KeepAlive = _options.KeepAlive
            });
    }

    private async Task<MemoryLoadContextResponse> LoadContextAsync(int turn)
    {
        var response = await _routerProxy.PostAsync<MemoryLoadContextRequest, MemoryLoadContextResponse>(
            "session_store",
            "/memory/load_context",
            new MemoryLoadContextRequest(turn, _options.RecentMessageCount));
        if (response.Payload is null || !response.Payload.Ok)
        {
            throw new InvalidOperationException("Failed to load memory context: " + response.RawBody);
        }

        return response.Payload;
    }

    private async Task PersistStepAsync(MemoryPersistStepRequest request)
    {
        var response = await _routerProxy.PostAsync<MemoryPersistStepRequest, MemoryPersistStepResponse>(
            "session_store",
            "/memory/persist_step",
            request);
        if (response.Payload is null || !response.Payload.Ok)
        {
            throw new InvalidOperationException("Failed to persist memory step: " + response.RawBody);
        }
    }

    private async Task SeedCoreMemoryIfEmptyAsync(string gameProjectId)
    {
        var response = await _routerProxy.PostAsync<MemoryBlocksGetAllRequest, MemoryBlocksGetAllResponse>(
            "session_store",
            "/memory/blocks/get_all",
            new MemoryBlocksGetAllRequest(true));
        if (response.Payload is null || !response.Payload.Ok)
        {
            throw new InvalidOperationException("Failed to inspect existing memory blocks: " + response.RawBody);
        }

        if (response.Payload.Blocks.Count > 0)
        {
            return;
        }

        foreach (var block in BuildSeedBlocks(gameProjectId))
        {
            var upsert = await _routerProxy.PostAsync<MemoryBlockUpsertRequest, MemoryBlockUpsertResponse>(
                "session_store",
                "/memory/blocks/upsert",
                new MemoryBlockUpsertRequest(block));
            if (upsert.Payload is null || !upsert.Payload.Ok)
            {
                throw new InvalidOperationException($"Failed to seed memory block '{block.Label}': {upsert.RawBody}");
            }
        }
    }
    #endregion

    #region Helpers
    private static MemoryBlockDto FindBlock(MemoryLoadContextResponse memoryContext, string label)
    {
        var block = memoryContext.Blocks.FirstOrDefault(block => string.Equals(block.Label, label, StringComparison.OrdinalIgnoreCase));
        return block ?? throw new InvalidOperationException($"Unknown memory block '{label}'.");
    }

    private IReadOnlyList<MemoryBlockDto> BuildSeedBlocks(string gameProjectId)
    {
        var instructionsPath = Path.Combine(_configuration.RepositoryRoot, "game_projects", gameProjectId, "system", "instructions.md");
        var instructions = File.Exists(instructionsPath)
            ? File.ReadAllText(instructionsPath)
            : "No project instructions were found.";

        return
        [
            new("persona", "GM voice and behavior.", "You are a memory-managed game master for MorpheusEngine.", 2000, false),
            new("campaign_rules", "Game-specific rules and constraints.", instructions, Math.Max(4000, instructions.Length + 256), false),
            new("player", "Durable facts and preferences about the player or player character.", "", 2000, false),
            new("current_scene", "Compact active situation.", "No scene has been established yet.", 2000, false),
            new("objectives", "Active narrative goals and unresolved threads.", "No active objectives recorded yet.", 2000, false),
            new("style", "Output constraints, tone, and formatting.", "Return concise, vivid second-person narration.", 1200, false),
            new("world_summary", "Rolling high-level summary of the run.", "The run has just begun.", 3000, false)
        ];
    }

    private string LoadAgentPrompt(string gameProjectId)
    {
        var path = Path.Combine(_configuration.RepositoryRoot, "game_projects", gameProjectId, "system", "agent_prompt.md");
        if (!File.Exists(path))
        {
            throw new FileNotFoundException($"Agent prompt file is required at '{path}'.", path);
        }

        return File.ReadAllText(path);
    }

    private JsonElement LoadActionSchema()
    {
        var path = Path.Combine(_configuration.RepositoryRoot, "docs", "schemas", "memory_director_action.schema.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        return doc.RootElement.Clone();
    }

    private static bool TryParseAction(string rawResponse, out AgentAction action, out string error)
    {
        action = new AgentAction(string.Empty, string.Empty, default);
        error = string.Empty;
        try
        {
            using var doc = JsonDocument.Parse(rawResponse);
            var root = doc.RootElement;
            var thought = RequireString(root, "thought");
            var tool = RequireString(root, "tool");
            if (!root.TryGetProperty("arguments", out var arguments) || arguments.ValueKind != JsonValueKind.Object)
            {
                error = "Action must include object property 'arguments'.";
                return false;
            }

            action = new AgentAction(thought, tool, arguments.Clone());
            return true;
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException)
        {
            error = e.Message;
            return false;
        }
    }

    private static string RequireString(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var property) || property.ValueKind != JsonValueKind.String)
        {
            throw new InvalidOperationException($"Expected string property '{propertyName}'.");
        }

        var value = property.GetString();
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidOperationException($"Expected non-empty string property '{propertyName}'.");
        }

        return value;
    }

    private static string CanonicalizeJson(JsonElement element) =>
        JsonSerializer.Serialize(element);

    private string TruncateToolResult(string value)
    {
        if (value.Length <= _options.MaxToolResultChars)
        {
            return value;
        }

        return value[.._options.MaxToolResultChars] + "\n[tool result truncated]";
    }

    private static string MapRoleForChat(string role) => role switch
    {
        "player" => "user",
        "assistant" => "assistant",
        _ => "user"
    };

    private static string FormatMemoryMessage(AgentMessageDto message) =>
        message.Role == "tool"
            ? $"Tool result ({message.ToolName ?? message.MessageType}): {message.Content}"
            : message.Content;

    private static async Task<string> ReadRequestBodyAsync(HttpListenerContext context)
    {
        using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
        return await reader.ReadToEndAsync();
    }

    private static async Task RespondJsonAsync(HttpListenerContext context, int statusCode, object payload)
    {
        var response = context.Response;
        response.StatusCode = statusCode;
        response.ContentType = "application/json";
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload));
        response.ContentLength64 = bytes.LongLength;
        await response.OutputStream.WriteAsync(bytes);
        response.OutputStream.Close();
    }
    #endregion
}
