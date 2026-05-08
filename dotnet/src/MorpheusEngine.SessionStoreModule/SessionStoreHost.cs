using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the session_store module: per-run SQLite open, schema bootstrap, run lifecycle, and turn persist.
/// POST /persist_turn targets the run established by the last successful host POST /initialize on this process (one run per process).
/// Essentially a wrapper for RunPersistence which does the actual work. This is just a wrapper that handles the HTTP messaging.
/// </summary>
public sealed class SessionStoreHost
{
    #region Private data

    private readonly HttpListener _listener = new();
    private readonly EngineConfiguration _configuration;
    private readonly RunPersistence _persistence;
    private readonly HttpClient _httpClient;
    private readonly RouterProxyClient _routerProxy;
    private readonly object _sessionLock = new();
    private volatile bool _initializing;
    private bool _shutdownRequested = false;
    /// <summary>Trimmed game project id from the last successful POST /initialize; empty until then.</summary>
    private string _boundGameProjectId = string.Empty;
    /// <summary>Trimmed run id from the last successful POST /initialize; empty until then.</summary>
    private string _boundRunId = string.Empty;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    #endregion

    public SessionStoreHost()
        : this(
            EngineConfigLoader.GetConfiguration(),
            new HttpClient
            {
                Timeout = TimeSpan.FromSeconds(60)
            })
    {
    }

    internal SessionStoreHost(EngineConfiguration configuration, HttpClient httpClient)
    {
        _configuration = configuration ?? throw new ArgumentNullException(nameof(configuration));
        _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
        _persistence = new RunPersistence(_configuration.RepositoryRoot);
        _routerProxy = new RouterProxyClient(_httpClient, _configuration, "session_store", JsonOptions);
    }

    #region Public methods

    private bool IsRunBound => !string.IsNullOrWhiteSpace(_boundRunId) && !string.IsNullOrWhiteSpace(_boundGameProjectId);

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
            Console.WriteLine("SessionStoreHost error: " + e.Message);
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
        var port = _configuration.GetRequiredListenPort("session_store");
        _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        _listener.Start();
        Console.WriteLine($"ready listen=http://127.0.0.1:{port}/");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
        _httpClient.Dispose();
        Console.WriteLine("SessionStore shut down.");
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
                await RespondJsonAsync(context, 200, new ModuleInfoResponse(true, "session_store"));
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                if (_initializing)
                {
                    await RespondJsonAsync(
                        context,
                        503,
                        new ModuleHealthResponse(false, "initializing", false));
                    return;
                }

                bool runBound;
                lock (_sessionLock)
                {
                    runBound = IsRunBound;
                }

                if (runBound)
                {
                    await RespondJsonAsync(context, 200, new ModuleHealthResponse(true, "healthy", true));
                    return;
                }

                await RespondJsonAsync(context, 200, new ModuleHealthResponse(false, "awaiting_initialize", false));
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
                await HandleRequest_bindRun(context);
                return;
            }

            if (path.Equals("/persist_turn", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleRequest_persistTurn(context);
                return;
            }

            if (path.Equals("/memory/load_context", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryLoadContextRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.LoadMemoryContext(gameProjectId, runId, request, CreateMemoryBudget(request.MaxFullMessages)));
                return;
            }

            if (path.Equals("/memory/persist_step", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryPersistStepRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.PersistMemoryStep(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/recall_search", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryRecallSearchRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.SearchRecall(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/archival_search", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryArchivalSearchRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.SearchArchival(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/archival_upsert", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryArchivalUpsertRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.UpsertArchivalPassage(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/summaries/recent", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemorySummariesRecentRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.GetRecentSummaries(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/recall/compact", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryCompactRecallRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.CompactRecall(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/blocks/get_all", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryBlocksGetAllRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.GetMemoryBlocks(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/blocks/upsert", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryBlockUpsertRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.UpsertMemoryBlock(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/messages/recent", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryMessagesRecentRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.GetRecentMessages(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/messages/append", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryMessageAppendRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.AppendMessage(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/mutations/append", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryMutationAppendRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.AppendMutation(gameProjectId, runId, request));
                return;
            }

            if (path.Equals("/memory/snapshot/latest", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemorySnapshotLatestRequest>(
                    context,
                    (gameProjectId, runId, _) => _persistence.GetLatestSnapshot(gameProjectId, runId));
                return;
            }

            if (path.Equals("/memory/pipeline_events/recent", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleMemoryRequest<MemoryPipelineEventsRecentRequest>(
                    context,
                    (gameProjectId, runId, request) => _persistence.GetRecentPipelineEvents(gameProjectId, runId, request));
                return;
            }

            await RespondJsonAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            Console.WriteLine("SessionStore unhandled request error: " + e.Message);
            if (context.Response.OutputStream.CanWrite)
            {
                await RespondJsonAsync(context, 500, new ErrorResponse(false, "Unhandled session_store error.", e.Message));
            }
        }
    }

    private async Task HandleRequest_bindRun(HttpListenerContext context)
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

        if (request is null
            || string.IsNullOrWhiteSpace(request.GameProjectId)
            || string.IsNullOrWhiteSpace(request.RunId))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty gameProjectId and runId."));
            return;
        }

        _initializing = true;
        try
        {
            try
            {
                InitializeModuleResponse response;
                lock (_sessionLock)
                {
                    response = _persistence.InitializeRun(request.GameProjectId.Trim(), request.RunId.Trim());
                }

                await SeedArchivalLoreAsync(request.GameProjectId.Trim(), request.RunId.Trim());

                lock (_sessionLock)
                {
                    _boundGameProjectId = request.GameProjectId.Trim();
                    _boundRunId = request.RunId.Trim();
                }

                await RespondJsonAsync(context, 200, response);
            }
            catch (ArgumentException e)
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
            }
            catch (Exception e)
            {
                await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to initialize run store.", e.Message));
            }
        }
        finally
        {
            _initializing = false;
        }
    }

    private async Task HandleRequest_persistTurn(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        TurnPersistRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<TurnPersistRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.PlayerInput)
            || string.IsNullOrWhiteSpace(request.DirectorResponseBody))
        {
            await RespondJsonAsync(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty turn, playerInput, and directorResponseBody."));
            return;
        }

        try
        {
            TurnPersistResponse? response = null;
            bool missingBound;
            lock (_sessionLock)
            {
                missingBound = string.IsNullOrEmpty(_boundGameProjectId) || string.IsNullOrEmpty(_boundRunId);
                if (!missingBound)
                {
                    response = _persistence.PersistTurn(_boundGameProjectId, _boundRunId, request);
                }
            }

            if (missingBound)
            {
                await RespondJsonAsync(
                    context,
                    400,
                    new ErrorResponse(
                        false,
                        "No bound run: the host must bind the run on this session_store process before POST /persist_turn."));
                return;
            }

            await RespondJsonAsync(context, 200, response!);
        }
        catch (ArgumentException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
        }
        catch (InvalidOperationException e)
        {
            await RespondJsonAsync(context, 409, new ErrorResponse(false, e.Message));
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to persist turn.", e.Message));
        }
    }

    private async Task SeedArchivalLoreAsync(string gameProjectId, string runId)
    {
        var candidates = _persistence.BuildArchivalLoreSeedCandidates(gameProjectId, runId);
        if (candidates.Count == 0)
        {
            return;
        }

        var embeddingsOptions = _configuration.GetRequiredGenericEmbeddingsModule().EmbeddingsOptions
            ?? throw new InvalidOperationException("generic_embeddings module must expose embeddings options for archival lore seeding.");
        var embeddingResponse = await _routerProxy.PostAsync<EmbeddingRequest, EmbeddingResponse>(
            "generic_embeddings",
            "/embed",
            new EmbeddingRequest(embeddingsOptions.DefaultEmbeddingModel, candidates.Select(static candidate => candidate.Content).ToArray()));
        if (embeddingResponse.Payload is null || !embeddingResponse.Payload.Ok)
        {
            throw new InvalidOperationException("Archival lore seeding failed to embed lore rows: " + embeddingResponse.RawBody);
        }

        if (embeddingResponse.Payload.Vectors.Count != candidates.Count)
        {
            throw new InvalidOperationException("Archival lore seeding received a mismatched embedding count.");
        }

        lock (_sessionLock)
        {
            foreach (var vector in embeddingResponse.Payload.Vectors.OrderBy(static vector => vector.Index))
            {
                if (vector.Index < 0 || vector.Index >= candidates.Count)
                {
                    throw new InvalidOperationException("Archival lore seeding received an out-of-range embedding index.");
                }

                var candidate = candidates[vector.Index];
                var passage = new ArchivalPassageDto(
                    candidate.Id,
                    candidate.Scope,
                    candidate.Source,
                    candidate.Content,
                    candidate.Tags,
                    candidate.MetadataJson,
                    embeddingResponse.Payload.Model,
                    embeddingResponse.Payload.Dimensions,
                    vector.Vector);
                _ = _persistence.UpsertArchivalPassage(gameProjectId, runId, new MemoryArchivalUpsertRequest(passage));
            }
        }
    }

    private async Task HandleMemoryRequest<TRequest>(
        HttpListenerContext context,
        Func<string, string, TRequest, object> responseFactory)
        where TRequest : class
    {
        var body = await ReadRequestBodyAsync(context);
        TRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<TRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload."));
            return;
        }

        try
        {
            object? response = null;
            bool missingBound;
            lock (_sessionLock)
            {
                missingBound = !IsRunBound;
                if (!missingBound)
                {
                    response = responseFactory(_boundGameProjectId, _boundRunId, request);
                }
            }

            if (missingBound)
            {
                await RespondJsonAsync(
                    context,
                    400,
                    new ErrorResponse(
                        false,
                        "No bound run: the host must bind the run on this session_store process before memory endpoints."));
                return;
            }

            await RespondJsonAsync(context, 200, response!);
        }
        catch (ArgumentException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
        }
        catch (InvalidOperationException e)
        {
            await RespondJsonAsync(context, 409, new ErrorResponse(false, e.Message));
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to process memory endpoint.", e.Message));
        }
    }

    private MemoryBudgetDto CreateMemoryBudget(int maxFullMessages)
    {
        var llmProvider = _configuration.GetRequiredGenericLlmProviderModule();
        if (llmProvider.GenericLlmProviderOptions is null)
        {
            throw new InvalidOperationException("Generic LLM provider options are required to derive the memory budget.");
        }

        var numCtx = llmProvider.GenericLlmProviderOptions.NumCtx;
        var maxToolResultChars = _configuration.GetRequiredGenericDirectorModule().MemoryDirectorOptions?.MaxToolResultChars ?? 4000;
        return new MemoryBudgetDto(numCtx, numCtx * 70 / 100, maxFullMessages, maxToolResultChars);
    }

    #endregion

    #region Helpers
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
