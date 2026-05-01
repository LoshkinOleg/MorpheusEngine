using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the director module: one in-process run at a time, system prompt from game project files, LLM via router proxy to generic_llm_provider /chat.
/// </summary>
public sealed class Director
{
    #region Nested types

    /// <summary>One in-memory chat message (mirrors Ollama roles used in <see cref="ChatGenerateRequest.ChatMessageDto"/>).</summary>
    private sealed record ChatMessage(string Role, string Content);

    #endregion

    #region Private data

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration(); // Note: each module loads the config independantly because that's where the http ports are defined. Can't pass them via /initialize since that would require the HTTP client to already be listening on a port.

    // LLM proxy calls share the same per-request ceiling as LlmProvider_qwen's outbound HttpClient (60s). Model weights are primed inside the provider before /health reports initialized.
    private readonly HttpClient _httpClient = new()
    {
        Timeout = TimeSpan.FromSeconds(60)
    };

    private readonly RouterProxyClient _routerProxy;

    private readonly HttpListener _listener = new();
    private bool _shutdownRequested = false;

    /// <summary>Single-flight gate for POST /initialize and /message (one active run per Director process). Used to prevent concurrent module state mutating HTTP calls.</summary>
    private readonly SemaphoreSlim _sessionGate = new(1, 1);

    /// <summary>Set after successful POST /initialize; cleared only when the process restarts.</summary>
    private volatile bool _initialized = false;

    private volatile bool _initializing; // Set to true while /initialize processing is in flight.

    /// <summary>Conversation for the bound run; first entry is always system after POST /initialize.</summary>
    private List<ChatMessage>? _history = null;

    #endregion

    #region Public methods

    public Director()
    {
        _routerProxy = new RouterProxyClient(_httpClient, _configuration, "director", JsonOptions);
    }

    public async Task Run()
    {
        Initialize();

        try
        {
            while (!_shutdownRequested)
            {
                var context = await _listener.GetContextAsync();
                _ = ProcessQuery(context);
            }
        }
        catch (HttpListenerException e)
        {
            Console.WriteLine("Director error encountered: " + e.Message);
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
        var ports = EngineConfigLoader.GetPorts();
        var directorPort = ports.GetRequiredPort("director");
        _listener.Prefixes.Add($"http://127.0.0.1:{directorPort}/");
        _listener.Start();
        Console.WriteLine($"ready listen=http://127.0.0.1:{directorPort}/");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
        _httpClient.Dispose();
        _sessionGate.Dispose();
        Console.WriteLine("Director shut down.");
    }

    private async Task ProcessQuery(HttpListenerContext context)
    {
        try
        {
            if (context.Request.Url is null)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid request URL."));
                return;
            }

            var path = context.Request.Url.AbsolutePath;

            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 200, new ModuleInfoResponse(true, "director"));
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
            {
                if (_initializing)
                {
                    await Respond(context, 503, new ModuleHealthResponse(false, "initializing", false));
                    return;
                }

                if (_initialized)
                {
                    await Respond(context, 200, new ModuleHealthResponse(true, "healthy", true));
                    return;
                }

                await Respond(context, 200, new ModuleHealthResponse(false, "awaiting_initialize", false));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_shutdown(context);
                return;
            }

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_bindRun(context);
                return;
            }

            if (path.Equals("/message", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_message(context);
                return;
            }

            await Respond(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            Console.WriteLine("Director encountered unhandled request error: " + e.Message);
            if (context.Response.OutputStream.CanWrite)
            {
                await Respond(context, 500, new ErrorResponse(false, "Unhandled director error.", e.Message));
            }
        }
    }

    private async Task ProcessRequest_shutdown(HttpListenerContext context)
    {
        await Respond(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
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
    }

    private async Task ProcessRequest_bindRun(HttpListenerContext context)
    {
        if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
        {
            await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
            return;
        }

        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        InitializeModuleRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.RunId)
            || string.IsNullOrWhiteSpace(request.GameProjectId))
        {
            await Respond(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty runId and gameProjectId."));
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
                    await Respond(
                        context,
                        409,
                        new ErrorResponse(
                            false,
                            "Director already bound for this process; restart the Director module to start another run."));
                    return;
                }

                string systemContent;
                try
                {
                    systemContent = DirectorNarrationSystemPrompt.Build(_configuration.RepositoryRoot, request.GameProjectId.Trim());
                }
                catch (FileNotFoundException e)
                {
                    await Respond(context, 500, new ErrorResponse(false, e.Message, e.FileName));
                    return;
                }
                catch (InvalidOperationException e)
                {
                    await Respond(context, 500, new ErrorResponse(false, e.Message));
                    return;
                }

                _history = new List<ChatMessage> { new ChatMessage("system", systemContent) };
                _initialized = true;

                await Respond(context, 200, new InitializeModuleResponse(true));
            }
            finally
            {
                _sessionGate.Release();
            }
        }
        finally
        {
            _initializing = false;
        }
    }

    /// <summary>
    /// Deserializes <see cref="DirectorMessageRequest"/>; requires prior POST /initialize; builds chat messages for Ollama, proxies to LLM, appends user+assistant to history on success, returns <see cref="DirectorMessageResponse"/>.
    /// </summary>
    private async Task ProcessRequest_message(HttpListenerContext context)
    {
        if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
        {
            await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
            return;
        }

        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        DirectorMessageRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<DirectorMessageRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
        {
            await Respond(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty playerInput."));
            return;
        }

        if (request.Turn < 1)
        {
            await Respond(context, 400, new ErrorResponse(false, "Turn must be >= 1."));
            return;
        }

        var playerInput = request.PlayerInput.Trim();

        await _sessionGate.WaitAsync();
        try
        {
            if (!_initialized || _history is null)
            {
                await Respond(
                    context,
                    400,
                    new ErrorResponse(false, "Director run is not bound; the host must bind the run before calling /message."));
                return;
            }

            var history = _history;

            // Build outbound messages without mutating history until the LLM call succeeds (avoids orphan user rows on failure).
            var messagesForApi = new List<ChatGenerateRequest.ChatMessageDto>(history.Count + 1);
            foreach (var row in history)
            {
                messagesForApi.Add(new ChatGenerateRequest.ChatMessageDto(row.Role, row.Content));
            }

            messagesForApi.Add(new ChatGenerateRequest.ChatMessageDto("user", playerInput)); // Add new player input.

            var chatRequest = new ChatGenerateRequest { Messages = messagesForApi };

            RouterProxyResponse<ChatGenerateResponse> llmResponse;
            try
            {
                llmResponse = await _routerProxy.PostAsync<ChatGenerateRequest, ChatGenerateResponse>(
                    "generic_llm_provider",
                    "/chat",
                    chatRequest);
            }
            catch (Exception e)
            {
                Console.WriteLine("[Director] Router proxy request failed: " + e.Message);
                await Respond(context, 502, new ErrorResponse(false, "Failed to reach router proxy for LLM chat.", e.Message));
                return;
            }

            var llmBody = llmResponse.RawBody;
            if (llmResponse.StatusCode is < 200 or >= 300)
            {
                Console.WriteLine($"[Director] Router proxy returned {llmResponse.StatusCode}: {llmBody}");
                await Respond(
                    context,
                    502,
                    new ErrorResponse(
                        false,
                        "Router proxy did not return success for LLM chat.",
                        TruncateDetails(llmBody)));
                return;
            }

            if (!string.IsNullOrWhiteSpace(llmResponse.DeserializeError) || llmResponse.Payload is null)
            {
                Console.WriteLine("[Director] Invalid JSON from proxied LLM provider: " + llmResponse.DeserializeError);
                await Respond(
                    context,
                    422,
                    new ErrorResponse(false, "Proxied LLM response was not valid JSON.", llmResponse.DeserializeError));
                return;
            }

            var reponsePayload = llmResponse.Payload;
            if (!reponsePayload.Ok || string.IsNullOrWhiteSpace(reponsePayload.Response))
            {
                Console.WriteLine("[Director] Proxied LLM chat response missing assistant text.");
                await Respond(
                    context,
                    422,
                    new ErrorResponse(
                        false,
                        "LLM chat response was empty or missing 'response'.",
                        TruncateDetails(llmBody)));
                return;
            }

            var assistantText = reponsePayload.Response.Trim();
            history.Add(new ChatMessage("user", playerInput));
            history.Add(new ChatMessage("assistant", assistantText));

            await Respond(context, 200, new DirectorMessageResponse(true, assistantText));
        }
        finally
        {
            _sessionGate.Release();
        }
    }

    private static string? TruncateDetails(string? text, int maxLen = 2000)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        return text.Length <= maxLen ? text : text[..maxLen] + "…";
    }

    private async Task Respond(HttpListenerContext context, int statusCode, object payload)
    {
        var response = context.Response;
        response.StatusCode = statusCode;
        response.ContentType = "application/json";
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload, JsonOptions));
        response.ContentLength64 = bytes.LongLength;
        await response.OutputStream.WriteAsync(bytes);
        response.OutputStream.Close();
    }

    #endregion
}
