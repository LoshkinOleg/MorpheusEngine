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

    private sealed record CompiledContext(
        IReadOnlyList<ChatGenerateRequest.ChatMessageDto> Messages,
        MemoryContextAccountingDto Accounting);

    private sealed class ContextBudget
    {
        public int TargetChars { get; }

        public int RemainingChars { get; private set; }

        public ContextBudget(int targetChars)
        {
            TargetChars = Math.Max(0, targetChars);
            RemainingChars = TargetChars;
        }

        public int Consume(int requestedChars)
        {
            var allowed = Math.Min(Math.Max(0, requestedChars), RemainingChars);
            RemainingChars -= allowed;
            return allowed;
        }
    }
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
                var compiledContext = await CompileContextAsync(memoryContext);
                var llmResponse = await ChatAsync(compiledContext.Messages);
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
                        lastThought,
                        compiledContext.Accounting);
                    if (finalFromParse is not null)
                    {
                        await CompactRecallIfNeededAsync(request.Turn);
                        await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, finalFromParse));
                        return;
                    }

                    continue;
                }

                lastThought = action.Thought;
                var canonicalToolCall = action.Tool + ":" + CanonicalizeJson(action.Arguments);
                var toolResult = await ExecuteToolAsync(request.Turn, step, memoryContext, action);

                var assistantMessage = new AgentMessageDto(
                    request.Turn,
                    step,
                    "assistant",
                    action.Tool == "send_message" ? "send_message" : "tool_call",
                    llmResponse.Payload.Response,
                    action.Tool);
                var toolMessages = new List<AgentMessageDto> { assistantMessage };
                var contextAccounting = compiledContext.Accounting;
                if (action.Tool != "send_message")
                {
                    contextAccounting = AddToolResultTelemetry(contextAccounting, action.Tool, toolResult.ToolResultContent);
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
                        toolResult.BlockUpdates,
                        contextAccounting));

                if (toolResult.FinalMessage is not null)
                {
                    await CompactRecallIfNeededAsync(request.Turn);
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
                        await CompactRecallIfNeededAsync(request.Turn);
                        await RespondJsonAsync(context, 200, new DirectorMessageResponse(true, finalFromTool));
                        return;
                    }
                }
            }

            var final = await SynthesizeAndPersistFinalAsync(request.Turn, _options.MaxStepsPerTurn + 1, lastThought);
            await CompactRecallIfNeededAsync(request.Turn);
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
        MemoryContextAccountingDto? contextAccounting = null,
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
                    [],
                    contextAccounting));
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

    private async Task<ToolExecutionResult> ExecuteToolAsync(int turn, int step, MemoryLoadContextResponse memoryContext, AgentAction action)
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
                "recall_search" => await ExecuteRecallSearchAsync(action.Arguments),
                "archival_memory_insert" => await ExecuteArchivalMemoryInsertAsync(turn, step, action.Arguments),
                "archival_memory_search" => await ExecuteArchivalMemorySearchAsync(action.Arguments),
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

    private async Task<ToolExecutionResult> ExecuteRecallSearchAsync(JsonElement arguments)
    {
        var query = RequireString(arguments, "query").Trim();
        var limit = TryGetInt(arguments, "limit") is { } requestedLimit ? Math.Clamp(requestedLimit, 1, 20) : 5;
        var roles = TryGetStringArray(arguments, "roles");
        var response = await _routerProxy.PostAsync<MemoryRecallSearchRequest, MemoryRecallSearchResponse>(
            "session_store",
            "/memory/recall_search",
            new MemoryRecallSearchRequest(query, roles, limit));
        if (response.Payload is null || !response.Payload.Ok)
        {
            return new ToolExecutionResult(false, "Recall search failed: " + response.RawBody, [], [], null);
        }

        return new ToolExecutionResult(true, FormatSearchResults(response.Payload.Results), [], [], null);
    }

    private async Task<ToolExecutionResult> ExecuteArchivalMemorySearchAsync(JsonElement arguments)
    {
        var query = RequireString(arguments, "query").Trim();
        var topK = TryGetInt(arguments, "topK") is { } requestedTopK ? Math.Clamp(requestedTopK, 1, 20) : 5;
        var tags = TryGetStringArray(arguments, "tags");
        var embedding = await EmbedTextAsync(query);
        var response = await _routerProxy.PostAsync<MemoryArchivalSearchRequest, MemoryArchivalSearchResponse>(
            "session_store",
            "/memory/archival_search",
            new MemoryArchivalSearchRequest(query, tags, topK, embedding.Vector, embedding.Model));
        if (response.Payload is null || !response.Payload.Ok)
        {
            return new ToolExecutionResult(false, "Archival search failed: " + response.RawBody, [], [], null);
        }

        return new ToolExecutionResult(true, FormatSearchResults(response.Payload.Results), [], [], null);
    }

    private async Task<ToolExecutionResult> ExecuteArchivalMemoryInsertAsync(int turn, int step, JsonElement arguments)
    {
        var content = RequireString(arguments, "content").Trim();
        var scope = TryGetString(arguments, "scope")?.Trim().ToLowerInvariant() ?? "run";
        if (scope is not ("project" or "run"))
        {
            throw new InvalidOperationException("archival_memory_insert scope must be 'project' or 'run'.");
        }

        var source = TryGetString(arguments, "source")?.Trim() ?? "memory_director";
        var tags = TryGetStringArray(arguments, "tags") ?? ["agent"];
        var embedding = await EmbedTextAsync(content);
        var passage = new ArchivalPassageDto(
            "agent:" + Guid.NewGuid().ToString("N"),
            scope,
            source,
            content,
            tags,
            JsonSerializer.Serialize(new { turn, stepNumber = step, insertedBy = "memory_director" }),
            embedding.Model,
            embedding.Vector.Count,
            embedding.Vector);
        var response = await _routerProxy.PostAsync<MemoryArchivalUpsertRequest, MemoryArchivalUpsertResponse>(
            "session_store",
            "/memory/archival_upsert",
            new MemoryArchivalUpsertRequest(passage));
        if (response.Payload is null || !response.Payload.Ok)
        {
            return new ToolExecutionResult(false, "Archival insert failed: " + response.RawBody, [], [], null);
        }

        var mutation = new MemoryMutationDto(
            turn,
            step,
            "archival_memory_insert",
            "archival:" + response.Payload.Passage.Id,
            null,
            JsonSerializer.Serialize(response.Payload.Passage));
        return new ToolExecutionResult(
            true,
            JsonSerializer.Serialize(new { inserted = response.Payload.Passage.Id, scope = response.Payload.Passage.Scope }),
            [],
            [mutation],
            null);
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

    private async Task<CompiledContext> CompileContextAsync(MemoryLoadContextResponse memoryContext)
    {
        var system = new StringBuilder();
        var budget = CreateContextBudget(memoryContext);
        var omissions = new List<string>();
        var items = new List<MemoryContextItemDto>();
        AppendWithinBudget(system, _agentPrompt, budget, omissions, items, "agent_prompt", "system");
        AppendWithinBudget(system, Environment.NewLine, budget, omissions, items, "separator", "system");
        AppendWithinBudget(system, "You must answer with exactly one JSON action matching the provided schema. Do not include markdown.\n", budget, omissions, items, "tool_rule_intro", "system");
        AppendWithinBudget(system, "Tool rules: send_message is terminal. Memory-edit, snapshot, recall_search, and archival memory tools are non-terminal.\n", budget, omissions, items, "tool_rules", "system");
        AppendWithinBudget(system, "Use recall_search when the player references prior turns or when recent context is insufficient.\n", budget, omissions, items, "recall_rule", "system");
        AppendWithinBudget(system, "Use archival_memory_search for durable lore/facts. Use archival_memory_insert only for stable facts worth future semantic retrieval.\n", budget, omissions, items, "archival_rule", "system");
        AppendWithinBudget(system, "Core memory blocks:\n", budget, omissions, items, "core_header", "core_header");
        foreach (var block in memoryContext.Blocks.OrderBy(static block => block.Label, StringComparer.OrdinalIgnoreCase))
        {
            AppendWithinBudget(system, $"[{block.Label}] {block.Description}\n{block.Value}\n", budget, omissions, items, "core:" + block.Label, "core");
        }

        if (memoryContext.Summaries is { Count: > 0 })
        {
            AppendWithinBudget(system, "Compacted recall summaries:\n", budget, omissions, items, "summaries_header", "summary_header");
            foreach (var summary in memoryContext.Summaries)
            {
                AppendWithinBudget(system, $"Turns {summary.StartTurn}-{summary.EndTurn}: {summary.Summary}\n", budget, omissions, items, "summary:" + summary.StartTurn + "-" + summary.EndTurn, "summary");
            }
        }

        AppendWithinBudget(system, "Latest snapshot:\n", budget, omissions, items, "snapshot_header", "snapshot_header");
        AppendWithinBudget(system, memoryContext.LatestSnapshot.WorldStateJson + "\n", budget, omissions, items, "world_snapshot", "snapshot");
        AppendWithinBudget(system, memoryContext.LatestSnapshot.ViewStateJson + "\n", budget, omissions, items, "view_snapshot", "snapshot");

        var recentMessages = new List<ChatGenerateRequest.ChatMessageDto>();
        var recentItemIndexes = new List<int>();
        foreach (var message in memoryContext.RecentMessages)
        {
            var formatted = FormatMemoryMessage(message);
            var label = $"message:{message.Turn}:{message.StepNumber}:{message.Role}";
            if (budget.RemainingChars <= 0)
            {
                omissions.Add(label);
                items.Add(new MemoryContextItemDto(label, "recent_message", "omitted", formatted.Length, 0, "context_budget"));
                continue;
            }

            var allowed = budget.Consume(formatted.Length);
            var status = allowed == formatted.Length ? "included" : "truncated";
            var reason = allowed == formatted.Length ? null : "context_budget";
            var itemIndex = items.Count;
            items.Add(new MemoryContextItemDto(label, "recent_message", status, formatted.Length, allowed, reason));
            recentItemIndexes.Add(itemIndex);
            recentMessages.Add(new ChatGenerateRequest.ChatMessageDto(MapRoleForChat(message.Role), TruncateWithSentinel(formatted, allowed)));
            if (reason is not null)
            {
                omissions.Add(label);
            }
        }

        var messages = BuildCompiledMessages(system.ToString(), omissions, recentMessages);
        var tokenCount = await CountTokensAsync(FlattenMessagesForTokenCount(messages));
        if (tokenCount.Exact && tokenCount.EstimatedTokens > memoryContext.Budget.TargetContextTokens && recentMessages.Count > 0)
        {
            var ratio = Math.Max(1.0, tokenCount.EstimatedTokens / (double)Math.Max(1, FlattenMessagesForTokenCount(messages).Length));
            while (recentMessages.Count > 0
                   && (int)Math.Ceiling(FlattenMessagesForTokenCount(BuildCompiledMessages(system.ToString(), omissions, recentMessages)).Length * ratio) > memoryContext.Budget.TargetContextTokens)
            {
                var removeIndex = recentMessages.Count - 1;
                recentMessages.RemoveAt(removeIndex);
                var itemIndex = recentItemIndexes[removeIndex];
                recentItemIndexes.RemoveAt(removeIndex);
                var item = items[itemIndex];
                items[itemIndex] = item with { Status = "omitted", IncludedChars = 0, Reason = "exact_token_budget" };
                omissions.Add(item.Label);
            }

            messages = BuildCompiledMessages(system.ToString(), omissions, recentMessages);
            tokenCount = await CountTokensAsync(FlattenMessagesForTokenCount(messages));
        }

        if (tokenCount.Exact && tokenCount.EstimatedTokens > memoryContext.Budget.TargetContextTokens)
        {
            items.Add(new MemoryContextItemDto(
                "compiled_prompt",
                "budget",
                "over_target",
                FlattenMessagesForTokenCount(messages).Length,
                FlattenMessagesForTokenCount(messages).Length,
                "core_system_exceeds_target",
                tokenCount.EstimatedTokens,
                tokenCount.Exact));
            omissions.Add("compiled_prompt:over_target");
        }

        var finalChars = FlattenMessagesForTokenCount(messages).Length;
        var accounting = new MemoryContextAccountingDto(
            finalChars,
            budget.TargetChars,
            omissions.Distinct(StringComparer.Ordinal).ToArray(),
            memoryContext.Budget.NumCtx,
            memoryContext.Budget.TargetContextTokens,
            tokenCount.EstimatedTokens,
            tokenCount.Exact,
            items);
        return new CompiledContext(messages, accounting);
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

    private async Task<TokenCountResponse> CountTokensAsync(string text)
    {
        var provider = _configuration.GetRequiredGenericLlmProviderModule();
        var model = provider.QwenOptions?.OllamaModel ?? string.Empty;
        var response = await _routerProxy.PostAsync<TokenCountRequest, TokenCountResponse>(
            "generic_llm_provider",
            "/token_count",
            new TokenCountRequest(model, text));
        if (response.Payload is not null && response.Payload.Ok)
        {
            return response.Payload;
        }

        return new TokenCountResponse(true, model, EstimateTokensFromChars(text.Length), false);
    }

    private async Task<(string Model, IReadOnlyList<float> Vector)> EmbedTextAsync(string text)
    {
        var embeddingsOptions = _configuration.GetRequiredGenericEmbeddingsModule().EmbeddingsOptions
            ?? throw new InvalidOperationException("generic_embeddings module must expose embeddings options.");
        var response = await _routerProxy.PostAsync<EmbeddingRequest, EmbeddingResponse>(
            "generic_embeddings",
            "/embed",
            new EmbeddingRequest(embeddingsOptions.DefaultEmbeddingModel, [text]));
        if (response.Payload is null || !response.Payload.Ok)
        {
            throw new InvalidOperationException("Embedding request failed: " + response.RawBody);
        }

        if (response.Payload.Vectors.Count != 1 || response.Payload.Vectors[0].Index != 0)
        {
            throw new InvalidOperationException("Embedding response for one text must contain exactly one vector at index 0.");
        }

        return (response.Payload.Model, response.Payload.Vectors[0].Vector);
    }

    private async Task<MemoryLoadContextResponse> LoadContextAsync(int turn)
    {
        var response = await _routerProxy.PostAsync<MemoryLoadContextRequest, MemoryLoadContextResponse>(
            "session_store",
            "/memory/load_context",
            new MemoryLoadContextRequest(turn, _options.MaxFullMessages));
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

    private async Task CompactRecallIfNeededAsync(int turn)
    {
        var recent = await _routerProxy.PostAsync<MemoryMessagesRecentRequest, MemoryMessagesRecentResponse>(
            "session_store",
            "/memory/messages/recent",
            new MemoryMessagesRecentRequest(Math.Max(_options.MaxFullMessages * 3, _options.MaxFullMessages + 1), null));
        if (recent.Payload is null || !recent.Payload.Ok)
        {
            throw new InvalidOperationException("Failed to inspect messages for compaction: " + recent.RawBody);
        }

        var compactable = recent.Payload.Messages
            .Where(message => message.Turn < turn)
            .OrderBy(message => message.Turn)
            .ThenBy(message => message.StepNumber)
            .ToArray();
        if (compactable.Length <= _options.MaxFullMessages)
        {
            return;
        }

        var messagesToSummarize = compactable.Take(compactable.Length - _options.MaxFullMessages).ToArray();
        if (messagesToSummarize.Length == 0)
        {
            return;
        }

        var startTurn = messagesToSummarize.Min(static message => message.Turn);
        var endTurn = messagesToSummarize.Max(static message => message.Turn);
        var summary = BuildDeterministicSummary(messagesToSummarize);
        var compact = await _routerProxy.PostAsync<MemoryCompactRecallRequest, MemoryCompactRecallResponse>(
            "session_store",
            "/memory/recall/compact",
            new MemoryCompactRecallRequest(
                startTurn,
                endTurn,
                summary,
                messagesToSummarize.Length,
                JsonSerializer.Serialize(new { reason = "post_turn_budget", turn })));
        if (compact.Payload is null || !compact.Payload.Ok)
        {
            throw new InvalidOperationException("Failed to persist recall compaction: " + compact.RawBody);
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

    private static int? TryGetInt(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var property))
        {
            return null;
        }

        return property.ValueKind == JsonValueKind.Number && property.TryGetInt32(out var value)
            ? value
            : throw new InvalidOperationException($"Expected integer property '{propertyName}'.");
    }

    private static string? TryGetString(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var property))
        {
            return null;
        }

        return property.ValueKind == JsonValueKind.String
            ? property.GetString()
            : throw new InvalidOperationException($"Expected string property '{propertyName}'.");
    }

    private static IReadOnlyList<string>? TryGetStringArray(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var property))
        {
            return null;
        }

        if (property.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidOperationException($"Expected array property '{propertyName}'.");
        }

        var values = new List<string>();
        foreach (var item in property.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(item.GetString()))
            {
                throw new InvalidOperationException($"Expected '{propertyName}' to contain only non-empty strings.");
            }

            values.Add(item.GetString()!.Trim());
        }

        return values;
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

    private MemoryContextAccountingDto AddToolResultTelemetry(MemoryContextAccountingDto accounting, string toolName, string toolResult)
    {
        var existingItems = accounting.Items?.ToList() ?? [];
        var status = toolResult.Length <= _options.MaxToolResultChars ? "included" : "truncated";
        var includedChars = Math.Min(toolResult.Length, _options.MaxToolResultChars);
        existingItems.Add(new MemoryContextItemDto(
            "tool_result:" + toolName,
            "tool_result",
            status,
            toolResult.Length,
            includedChars,
            status == "truncated" ? "max_tool_result_chars" : null,
            EstimateTokensFromChars(includedChars),
            false));
        var omissions = accounting.Omissions.ToList();
        if (status == "truncated")
        {
            omissions.Add("tool_result:" + toolName);
        }

        return accounting with
        {
            Omissions = omissions.Distinct(StringComparer.Ordinal).ToArray(),
            Items = existingItems
        };
    }

    private static ContextBudget CreateContextBudget(MemoryLoadContextResponse memoryContext)
    {
        var estimatedTargetChars = Math.Max(1000, memoryContext.Budget.TargetContextTokens * 4);
        return new ContextBudget(estimatedTargetChars);
    }

    private static void AppendWithinBudget(
        StringBuilder builder,
        string value,
        ContextBudget budget,
        List<string> omissions,
        List<MemoryContextItemDto> items,
        string label,
        string type)
    {
        var allowed = budget.Consume(value.Length);
        if (allowed == value.Length)
        {
            builder.Append(value);
            items.Add(new MemoryContextItemDto(label, type, "included", value.Length, allowed, null, EstimateTokensFromChars(value.Length), false));
            return;
        }

        if (allowed > 0)
        {
            builder.Append(TruncateWithSentinel(value, allowed));
            items.Add(new MemoryContextItemDto(label, type, "truncated", value.Length, allowed, "context_budget", EstimateTokensFromChars(allowed), false));
        }
        else
        {
            items.Add(new MemoryContextItemDto(label, type, "omitted", value.Length, 0, "context_budget", 0, false));
        }

        omissions.Add(label);
    }

    private static string TruncateWithSentinel(string value, int maxChars)
    {
        const string sentinel = "\n[omitted due to context budget]";
        if (value.Length <= maxChars)
        {
            return value;
        }

        if (maxChars <= sentinel.Length)
        {
            return sentinel;
        }

        return value[..(maxChars - sentinel.Length)] + sentinel;
    }

    private static IReadOnlyList<ChatGenerateRequest.ChatMessageDto> BuildCompiledMessages(
        string system,
        IReadOnlyList<string> omissions,
        IReadOnlyList<ChatGenerateRequest.ChatMessageDto> recentMessages)
    {
        var finalSystem = new StringBuilder(system);
        if (omissions.Count > 0)
        {
            finalSystem.AppendLine("[context omissions: " + string.Join("; ", omissions.Distinct(StringComparer.Ordinal)) + "]");
        }

        var messages = new List<ChatGenerateRequest.ChatMessageDto>
        {
            new("system", finalSystem.ToString())
        };
        messages.AddRange(recentMessages);
        return messages;
    }

    private static string FlattenMessagesForTokenCount(IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages)
    {
        var builder = new StringBuilder();
        foreach (var message in messages)
        {
            builder.Append(message.Role);
            builder.Append(": ");
            builder.AppendLine(message.Content);
        }

        return builder.ToString();
    }

    private static int EstimateTokensFromChars(int chars) =>
        Math.Max(1, (int)Math.Ceiling(chars / 4.0));

    private string FormatSearchResults(IReadOnlyList<MemorySearchResultDto> results)
    {
        if (results.Count == 0)
        {
            return "{\"results\":[]}";
        }

        var builder = new StringBuilder();
        builder.AppendLine("{\"results\":[");
        var usedChars = 0;
        var included = 0;
        foreach (var result in results)
        {
            var line = JsonSerializer.Serialize(new
            {
                result.Id,
                result.Source,
                result.Content,
                result.Score,
                result.MetadataJson
            });
            if (usedChars + line.Length > _options.MaxToolResultChars)
            {
                if (included > 0)
                {
                    builder.AppendLine(",");
                }

                builder.AppendLine(JsonSerializer.Serialize(new { omitted = results.Count - included, reason = "maxToolResultChars" }));
                break;
            }

            if (included > 0)
            {
                builder.AppendLine(",");
            }

            builder.Append(line);
            usedChars += line.Length;
            included++;
        }

        builder.AppendLine("]}");
        return builder.ToString();
    }

    private static string BuildDeterministicSummary(IReadOnlyList<AgentMessageDto> messages)
    {
        var builder = new StringBuilder();
        builder.Append($"Summary of turns {messages.Min(static message => message.Turn)}-{messages.Max(static message => message.Turn)}: ");
        foreach (var message in messages.Take(8))
        {
            builder.Append('[');
            builder.Append(message.Role);
            builder.Append(" t");
            builder.Append(message.Turn);
            builder.Append("] ");
            builder.Append(message.Content.Length <= 160 ? message.Content : message.Content[..160] + " [truncated]");
            builder.Append(' ');
        }

        if (messages.Count > 8)
        {
            builder.Append($"[{messages.Count - 8} additional messages omitted from deterministic summary]");
        }

        return builder.ToString().Trim();
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
