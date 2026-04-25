using System.Diagnostics;
using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    /// <summary>
    /// HTTP entrypoint for player-facing routes and for proxying allowlisted calls between modules.
    /// Configuration (ports, module endpoints) comes from <see cref="EngineConfiguration"/>.
    /// </summary>
    public class Router : IEngineRunBinder
    {
        #region Nested types

        /// <summary>
        /// Carrier for an outbound module call result: status, content type, and body as already received from the target.
        /// <see cref="sealed"/> + <see cref="record"/>: immutable value type; sealed documents that this type is not meant to be extended
        /// (subclassing a private nested type is already impossible from outside, but sealing keeps intent obvious and can help the compiler).
        /// </summary>
        private sealed record ForwardedModuleResult(int StatusCode, string ContentType, string Body)
        {
            public static ForwardedModuleResult FromError(int statusCode, string error, string? details = null) =>
                new(
                    statusCode,
                    "application/json",
                    JsonSerializer.Serialize(new ErrorResponse(false, error, details)));
        }

        #endregion

        #region Private data

        /// <summary>Accepts incoming HTTP requests for this process (router port from config).</summary>
        private readonly HttpListener _listener = new();

        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();

        /// <summary>Set when /shutdown is received or <see cref="RequestShutdown"/> is called; exits the accept loop.</summary>
        private bool _shutdownRequested;

        private static readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private bool _runBound = false;
        private volatile bool _initializing;
        private string _boundGameProjectId = string.Empty;
        private string _boundRunId = string.Empty;

        // Proxied module calls include LLM /chat; same ceiling as LlmProvider_qwen outbound calls (warm-up runs at provider startup).
        private static readonly HttpClient _httpClient = new()
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        /// <summary>
        /// Proxied responses from downstream modules must declare this media type (no silent fallback to JSON).
        /// </summary>
        private const string ExpectedProxiedResponseMediaType = "application/json";

        #endregion

        public bool IsRunBound => _runBound;

        public async Task Run()
        {
            Initialize();

            try
            {
                while (!_shutdownRequested)
                {
                    // GetContextAsync yields the thread while waiting; it does not burn a thread pool thread blocking.
                    // When a connection arrives, the await completes and we handle the request (fire-and-forget below).
                    var context = await _listener.GetContextAsync();
                    _ = ProcessQuery(context);
                }
            }
            catch (HttpListenerException e)
            {
                Console.WriteLine("Error encountered: " + e.Message);
            }
            finally
            {
                Shutdown();
            }
        }

        /// <summary>
        /// Registers the URL prefix HttpListener will bind to. Must match scheme, host, port, and optional path
        /// (here: loopback + router port + root path). Without a registered prefix, Start() cannot listen.
        /// </summary>
        private void Initialize()
        {
            var routerPort = _configuration.GetRequiredListenPort("router");
            _listener.Prefixes.Add($"http://127.0.0.1:{routerPort}/");
            _listener.Start();
            Console.WriteLine($"ready listen=http://127.0.0.1:{routerPort}/");
        }

        private async Task ProcessQuery(HttpListenerContext context)
        {
            try
            {
                if (context.Request.Url is null)
                {
                    await RespondAsync(context, 400, new ErrorResponse(false, "Invalid request URL."));
                    return;
                }

                var path = context.Request.Url.AbsolutePath;

                if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
                {
                    await RespondAsync(context, 200, new ModuleInfoResponse(true, "router"));
                    return;
                }

                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    if (_initializing)
                    {
                        await RespondAsync(
                            context,
                            503,
                            new ModuleHealthResponse(false, "initializing", false));
                        return;
                    }

                    if (_runBound)
                    {
                        await RespondAsync(context, 200, new ModuleHealthResponse(true, "healthy", true));
                        return;
                    }

                    await RespondAsync(context, 200, new ModuleHealthResponse(false, "awaiting_initialize", false));
                    return;
                }

                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await RespondAsync(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
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

                if (path.Equals("/turn", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_turn(context);
                    return;
                }

                if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_initialize(context);
                    return;
                }

                if (path.Equals("/proxy", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_proxy(context);
                    return;
                }

                Console.WriteLine("Request for router did not match any expected endpoints. Returning 404.");
                await RespondAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
            }
            catch (Exception e)
            {
                Console.WriteLine("Router encountered unhandled request error: " + e.Message);
                if (context.Response.OutputStream.CanWrite)
                {
                    await RespondAsync(context, 500, new ErrorResponse(false, "Unhandled router error.", e.Message));
                }
            }
        }

        public void RequestShutdown() => _shutdownRequested = true;

        public Task BindRunAsync(InitializeModuleRequest request, CancellationToken cancellationToken)
        {
            if (request is null
                || string.IsNullOrWhiteSpace(request.GameProjectId)
                || string.IsNullOrWhiteSpace(request.RunId))
            {
                throw new ArgumentException("Request must include non-empty gameProjectId and runId.", nameof(request));
            }

            cancellationToken.ThrowIfCancellationRequested();
            if (_runBound)
            {
                throw new InvalidOperationException("Router is already bound for this process; restart the router to bind another run.");
            }

            _boundGameProjectId = request.GameProjectId.Trim();
            _boundRunId = request.RunId.Trim();
            _runBound = true;
            Console.WriteLine($"[Router] Bound run runId={_boundRunId} gameProjectId={_boundGameProjectId}.");
            return Task.CompletedTask;
        }

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();
            Console.WriteLine("Router shut down.");
        }

        private async Task ProcessRequest_initialize(HttpListenerContext context)
        {
            if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
            {
                await RespondAsync(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
                return;
            }

            var body = await ReadRequestBodyAsync(context);

            if (!IsLoopbackRequest(context))
            {
                await RespondAsync(context, 403, new ErrorResponse(false, "POST /initialize is only allowed from loopback."));
                return;
            }

            InitializeModuleRequest? parsed;
            try
            {
                parsed = JsonSerializer.Deserialize<InitializeModuleRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (parsed is null
                || string.IsNullOrWhiteSpace(parsed.RunId)
                || string.IsNullOrWhiteSpace(parsed.GameProjectId))
            {
                await RespondAsync(
                    context,
                    400,
                    new ErrorResponse(false, "Request must include non-empty runId and gameProjectId."));
                return;
            }

            _initializing = true;
            try
            {
                try
                {
                    await BindRunAsync(parsed, CancellationToken.None);
                }
                catch (Exception e)
                {
                    await RespondAsync(context, 409, new ErrorResponse(false, e.Message));
                    return;
                }

                await RespondAsync(context, 200, new InitializeModuleResponse(true));
            }
            finally
            {
                _initializing = false;
            }
        }

        /// <summary>
        /// Player turn: call director with <see cref="DirectorMessageRequest"/>, then persist events + snapshot through session_store
        /// (which enforces sequencing). Returns a router-owned <see cref="TurnResponse"/> on success.
        /// </summary>
        private async Task ProcessRequest_turn(HttpListenerContext context)
        {
            var body = await ReadRequestBodyAsync(context);

            TurnRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<TurnRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null
                || string.IsNullOrWhiteSpace(request.PlayerInput))
            {
                await RespondAsync(
                    context,
                    400,
                    new ErrorResponse(false, "Turn request must include non-empty playerInput."));
                return;
            }

            if (!_runBound)
            {
                await RespondAsync(context, 503, new ErrorResponse(false, "Router run is not bound; the host must bind the run before POST /turn."));
                return;
            }

            if (request.Turn < 1)
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Turn must be >= 1."));
                return;
            }

            var turnStopwatch = Stopwatch.StartNew();
            var playerInputTrimmed = request.PlayerInput.Trim();
            var turnStartInner =
                $"=== TURN {request.Turn} START === runId={_boundRunId} gameProjectId={_boundGameProjectId} input='{TruncateMiddle(playerInputTrimmed)}'";
            Console.WriteLine(turnStartInner);

            var finalStatusCode = 500;
            try
            {
                var directorPayload = JsonSerializer.Serialize(
                    new DirectorMessageRequest(request.Turn, playerInputTrimmed));
                var directorResult = await ForwardModuleCallAsync(
                    "router",
                    "director",
                    "/message",
                    "POST",
                    directorPayload);

                if (directorResult.StatusCode is < 200 or >= 300)
                {
                    finalStatusCode = directorResult.StatusCode;
                    await WriteForwardedResultAsync(context, directorResult);
                    return;
                }

                DirectorMessageResponse? parsedDirector;
                try
                {
                    parsedDirector = JsonSerializer.Deserialize<DirectorMessageResponse>(directorResult.Body, _jsonOptions);
                }
                catch (JsonException)
                {
                    finalStatusCode = directorResult.StatusCode;
                    await WriteForwardedResultAsync(context, directorResult);
                    return;
                }

                if (parsedDirector is null || !parsedDirector.Ok || string.IsNullOrWhiteSpace(parsedDirector.Text))
                {
                    finalStatusCode = directorResult.StatusCode;
                    await WriteForwardedResultAsync(context, directorResult);
                    return;
                }

                var persistJson = JsonSerializer.Serialize(
                    new TurnPersistRequest(
                        request.Turn,
                        playerInputTrimmed,
                        directorResult.Body));

                var persistResult = await ForwardModuleCallAsync(
                    "router",
                    "session_store",
                    "/persist_turn",
                    "POST",
                    persistJson);

                if (persistResult.StatusCode != 200)
                {
                    finalStatusCode = persistResult.StatusCode;
                    Console.WriteLine(
                        "session_store /persist_turn failed after director /message succeeded; "
                        + "Director in-memory history may be ahead of SQLite.");
                    await WriteForwardedResultAsync(context, persistResult);
                    return;
                }

                TurnPersistResponse? persistBody;
                try
                {
                    persistBody = JsonSerializer.Deserialize<TurnPersistResponse>(persistResult.Body, _jsonOptions);
                }
                catch (JsonException e)
                {
                    finalStatusCode = 500;
                    await RespondAsync(
                        context,
                        500,
                        new ErrorResponse(false, "Session store returned invalid JSON for turn persistence.", e.Message));
                    return;
                }

                if (persistBody is null || !persistBody.Ok)
                {
                    finalStatusCode = 500;
                    await RespondAsync(
                        context,
                        500,
                        new ErrorResponse(false, "Session store reported persistence failure.", persistResult.Body));
                    return;
                }

                finalStatusCode = 200;
                await RespondAsync(
                    context,
                    200,
                    new TurnResponse(true, parsedDirector.Text.Trim()));
            }
            finally
            {
                turnStopwatch.Stop();
                var turnEndInner =
                    $"=== TURN {request.Turn} END === status={finalStatusCode} elapsedMs={turnStopwatch.ElapsedMilliseconds}";
                Console.WriteLine(turnEndInner);
            }
        }

        private static bool IsLoopbackRequest(HttpListenerContext context)
        {
            var ep = context.Request.RemoteEndPoint;
            return ep is null || IPAddress.IsLoopback(ep.Address);
        }

        /// <summary>
        /// Generic proxy: caller supplies target module key, path, HTTP method, and optional JSON body.
        /// Only pairs (path, method) that appear on that module in engine_config.json are allowed.
        /// </summary>
        private async Task ProcessRequest_proxy(HttpListenerContext context)
        {
            var body = await ReadRequestBodyAsync(context);

            ModuleProxyRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<ModuleProxyRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Invalid proxy request payload.", e.Message));
                return;
            }

            if (request is null
                || string.IsNullOrWhiteSpace(request.SourceModule)
                || string.IsNullOrWhiteSpace(request.TargetModule)
                || string.IsNullOrWhiteSpace(request.TargetPath))
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Proxy request must include sourceModule, targetModule, and targetPath."));
                return;
            }

            // Fail fast: method is required; do not default to POST.
            if (string.IsNullOrWhiteSpace(request.Method))
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Proxy request must include a non-empty 'method' field (GET or POST)."));
                return;
            }

            var method = request.Method.Trim().ToUpperInvariant();
            if (method != "GET" && method != "POST")
            {
                await RespondAsync(context, 400, new ErrorResponse(false, $"Unsupported proxy method '{request.Method}'. Only GET and POST are supported."));
                return;
            }

            var result = await ForwardModuleCallAsync(
                request.SourceModule.Trim(),
                request.TargetModule.Trim(),
                request.TargetPath,
                method,
                request.Body?.GetRawText());

            await WriteForwardedResultAsync(context, result);
        }

        /// <summary>
        /// Performs an allowlisted HTTP call to another module and returns its response for re-sending to the original client.
        /// </summary>
        /// <param name="sourceModule">Label for audit logs only (e.g. intent_extractor, player_ui).</param>
        /// <param name="targetModuleKey">port_key from config, e.g. llm_provider_qwen.</param>
        /// <param name="targetPath">Path on the target, e.g. /generate.</param>
        /// <param name="method">GET or POST (already normalized by callers).</param>
        /// <param name="requestBody">JSON string for POST; may be null/empty for POST with empty body.</param>
        private async Task<ForwardedModuleResult> ForwardModuleCallAsync(
            string sourceModule,
            string targetModuleKey,
            string targetPath,
            string method,
            string? requestBody)
        {
            var normalizedPath = EngineConfiguration.NormalizePath(targetPath);
            var methodUpper = method.Trim().ToUpperInvariant();

            var resolvedModuleKey = _configuration.ResolveProxyTargetModuleKey(targetModuleKey);
            var targetModule = _configuration.FindModule(resolvedModuleKey);
            if (targetModule is null)
            {
                return ForwardedModuleResult.FromError(400, $"Unknown target module '{resolvedModuleKey}'.");
            }

            var targetPort = _configuration.GetRequiredListenPort(targetModule.PortKey);

            // Allowlist: only (path, method) pairs declared on this module in engine_config.json may be reached through the proxy.
            // This blocks arbitrary SSRF-style forwarding even though the caller is on localhost.
            var endpoint = targetModule.Endpoints.FirstOrDefault(ep =>
                string.Equals(EngineConfiguration.NormalizePath(ep.Path), normalizedPath, StringComparison.OrdinalIgnoreCase)
                && string.Equals(ep.Method, methodUpper, StringComparison.OrdinalIgnoreCase));
            if (endpoint is null)
            {
                return ForwardedModuleResult.FromError(403, $"Proxy target '{targetModuleKey} {methodUpper} {normalizedPath}' is not allowed by configuration.");
            }

            var uri = $"http://127.0.0.1:{targetPort}{normalizedPath}";
            Console.WriteLine($"Proxied call: {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath}");

            try
            {
                using var outbound = new HttpRequestMessage(new HttpMethod(methodUpper), uri);
                if (methodUpper == "POST")
                {
                    outbound.Content = string.IsNullOrWhiteSpace(requestBody)
                        ? new ByteArrayContent(Array.Empty<byte>())
                        : new StringContent(requestBody, Encoding.UTF8, "application/json");
                }

                using var response = await _httpClient.SendAsync(outbound);
                var responseBody = await response.Content.ReadAsStringAsync();

                // Fail loud: downstream modules used through the proxy must return application/json (no ?? fallback).
                var mediaTypeHeader = response.Content.Headers.ContentType;
                if (mediaTypeHeader is null
                    || !string.Equals(mediaTypeHeader.MediaType, ExpectedProxiedResponseMediaType, StringComparison.OrdinalIgnoreCase))
                {
                    var actual = mediaTypeHeader?.MediaType ?? "(null)";
                    throw new InvalidOperationException(
                        $"Proxied module response must have Content-Type '{ExpectedProxiedResponseMediaType}'; received '{actual}'.");
                }

                Console.WriteLine(
                    $"Proxied call response: {targetModule.PortKey} -> {sourceModule} {methodUpper} {normalizedPath} => {(int)response.StatusCode}");

                // What the router's caller receives: same status code, content type, and body the router got from the target module.
                return new ForwardedModuleResult(
                    (int)response.StatusCode,
                    ExpectedProxiedResponseMediaType,
                    responseBody);
            }
            catch (Exception e) when (e is not InvalidOperationException)
            {
                Console.WriteLine(
                    $"Proxied call response: {targetModule.PortKey} -> {sourceModule} {methodUpper} {normalizedPath} => network_error: {e.Message}");
                return ForwardedModuleResult.FromError(
                    502,
                    $"Failed to reach target module '{targetModule.PortKey}'.",
                    e.Message);
            }
        }

        #region Helpers

        private static string TruncateForLog(string text, int maxLen = 160)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            return text.Length <= maxLen ? text : text[..maxLen] + "…";
        }

        /// <summary>Same shape as LlmProvider_qwen log previews: long text keeps head and tail with an ellipsis gap in the middle.</summary>
        private static string TruncateMiddle(string text, int headChars = 80, int tailChars = 60)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            if (headChars < 0 || tailChars < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(headChars), "headChars and tailChars must be >= 0.");
            }

            var max = headChars + tailChars + 5;
            if (text.Length <= max || headChars == 0 || tailChars == 0)
            {
                return TruncateForLog(text, maxLen: max);
            }

            return text[..headChars] + " ... " + text[^tailChars..];
        }

        /// <summary>
        /// Reads the request body without blocking a thread pool thread for the duration of the read:
        /// <see cref="StreamReader.ReadToEndAsync"/> is asynchronous I/O when the stream supports it.
        /// </summary>
        private async Task<string> ReadRequestBodyAsync(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            return await reader.ReadToEndAsync();
        }

        /// <summary>
        /// Writes a proxied module response verbatim (status, content-type, body) to the incoming HttpListener response.
        /// </summary>
        private async Task WriteForwardedResultAsync(HttpListenerContext context, ForwardedModuleResult result)
        {
            var response = context.Response;
            response.StatusCode = result.StatusCode;
            response.ContentType = result.ContentType;
            var payload = Encoding.UTF8.GetBytes(result.Body);
            response.ContentLength64 = payload.LongLength;
            await response.OutputStream.WriteAsync(payload);
            response.OutputStream.Close();
        }

        /// <summary>
        /// Router-native JSON responses: serializes a CLR object to JSON and always sets application/json.
        /// Used for /info, /health, errors, etc. — not for pass-through of another module's raw body.
        /// </summary>
        private async Task RespondAsync(HttpListenerContext context, int statusCode, object payload)
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
}
