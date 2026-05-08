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
    public class Router
    {
        #region Nested types

        /// <summary>
        /// Carrier for an outbound module call result: status, content type, and body as already received from the target.
        /// <see cref="sealed"/> + <see cref="record"/>: immutable value type; sealed documents that this type is not meant to be extended
        /// (subclassing a private nested type is already impossible from outside, but sealing keeps intent obvious and can help the compiler).
        /// </summary>
        internal sealed record ForwardedModuleResult(int StatusCode, string ContentType, string Body)
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

        private readonly EngineConfiguration _configuration;

        /// <summary>Set when /shutdown is received or <see cref="RequestShutdown"/> is called; exits the accept loop.</summary>
        private bool _shutdownRequested;

        private static readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private volatile bool _runBound = false;
        private volatile bool _initializing;
        private string _boundGameProjectId = string.Empty;
        private string _boundRunId = string.Empty;
        private EngineTurnPipelineInfo? _turnPipeline;

        // Proxied module calls include LLM /chat; same 60s ceiling as LlmProvider_qwen outbound calls (provider primes the model before initialized=true).
        private readonly HttpClient _httpClient;

        /// <summary>
        /// Proxied responses from downstream modules must declare this media type (no silent fallback to JSON).
        /// </summary>
        private const string ExpectedProxiedResponseMediaType = "application/json";

        #endregion

        public Router()
            : this(
                EngineConfigLoader.GetConfiguration(),
                new HttpClient
                {
                    Timeout = TimeSpan.FromSeconds(60)
                })
        {
        }

        internal Router(EngineConfiguration configuration, HttpClient httpClient)
        {
            _configuration = configuration ?? throw new ArgumentNullException(nameof(configuration));
            _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
        }

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

        /// <summary>Host-driven run binding (invoked from POST /initialize after JSON validation).</summary>
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

            var gameProjectId = request.GameProjectId.Trim();
            var runId = request.RunId.Trim();
            var manifest = GameProjectManifestLoader.Load(_configuration.RepositoryRoot, gameProjectId);
            var selectedPipeline = _configuration.GetRequiredTurnPipeline(manifest.TurnPipeline);
            var requiredForRun = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var module in _configuration.ModulesInfos)
            {
                if (module.RequiredByEngine)
                {
                    requiredForRun.Add(module.PortKey);
                }
            }

            foreach (var moduleKey in manifest.RequiredModules)
            {
                var trimmedModuleKey = moduleKey.Trim();
                var resolvedModuleKey = _configuration.ResolveProxyTargetModuleKey(trimmedModuleKey);
                if (_configuration.FindModule(resolvedModuleKey) is null)
                {
                    throw new InvalidOperationException($"Manifest required_modules contains unknown module or alias '{trimmedModuleKey}'.");
                }

                requiredForRun.Add(trimmedModuleKey);
                requiredForRun.Add(resolvedModuleKey);
            }

            foreach (var step in selectedPipeline.Steps)
            {
                var resolvedStepModuleKey = _configuration.ResolveProxyTargetModuleKey(step.TargetModule);
                var module = _configuration.FindModule(resolvedStepModuleKey);
                if (module is null)
                {
                    throw new InvalidOperationException(
                        $"Selected turn_pipeline '{selectedPipeline.Id}' references unknown module or alias '{step.TargetModule}'.");
                }

                if (!requiredForRun.Contains(step.TargetModule)
                    && !requiredForRun.Contains(resolvedStepModuleKey))
                {
                    throw new InvalidOperationException(
                        $"Selected turn_pipeline '{selectedPipeline.Id}' step '{step.Id}' references module '{step.TargetModule}', "
                        + "but that module is not required by the engine or the game project manifest.");
                }
            }

            _boundGameProjectId = gameProjectId;
            _boundRunId = runId;
            _turnPipeline = selectedPipeline;
            _runBound = true;

            Console.WriteLine($"[Router] Bound run runId={_boundRunId} gameProjectId={_boundGameProjectId} turnPipeline={selectedPipeline.Id}.");
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
        /// Player turn: execute the manifest-selected turn pipeline, then map the configured terminal step into
        /// the router-owned <see cref="TurnResponse"/> shape.
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
                var pipeline = _turnPipeline
                    ?? throw new InvalidOperationException("Router run is bound but no turn pipeline was selected.");
                var stepResults = new Dictionary<string, ForwardedModuleResult>(StringComparer.OrdinalIgnoreCase);
                ForwardedModuleResult? previousResult = null;

                foreach (var step in pipeline.Steps)
                {
                    var payload = RenderTurnPipelineStepBody(
                        step.BodyTemplate,
                        request.Turn,
                        playerInputTrimmed,
                        previousResult,
                        stepResults);

                    var stepResult = await ForwardModuleCallAsync(
                        "router",
                        step.TargetModule,
                        step.Path,
                        step.Method,
                        payload);
                    stepResults[step.Id] = stepResult;
                    previousResult = stepResult;

                    if (stepResult.StatusCode is < 200 or >= 300)
                    {
                        finalStatusCode = stepResult.StatusCode;
                        if (IsPersistTurnStep(step))
                        {
                            Console.WriteLine(
                                "session_store /persist_turn failed after director /message succeeded; "
                                + "Director in-memory history may be ahead of SQLite.");
                        }

                        if (!step.ContinueOnFailure)
                        {
                            await WriteForwardedResultAsync(context, stepResult);
                            return;
                        }
                    }

                    if (IsPersistTurnStep(step) && stepResult.StatusCode is >= 200 and < 300)
                    {
                        TurnPersistResponse? persistBody;
                        try
                        {
                            persistBody = JsonSerializer.Deserialize<TurnPersistResponse>(stepResult.Body, _jsonOptions);
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
                                new ErrorResponse(false, "Session store reported persistence failure.", stepResult.Body));
                            return;
                        }
                    }
                }

                if (!stepResults.TryGetValue(pipeline.ResponseMapping.SourceStep, out var terminalResult))
                {
                    throw new InvalidOperationException(
                        $"Turn pipeline '{pipeline.Id}' response_mapping.source_step '{pipeline.ResponseMapping.SourceStep}' did not execute.");
                }

                DirectorMessageResponse? parsedDirector;
                try
                {
                    parsedDirector = JsonSerializer.Deserialize<DirectorMessageResponse>(terminalResult.Body, _jsonOptions);
                }
                catch (JsonException)
                {
                    finalStatusCode = terminalResult.StatusCode;
                    await WriteForwardedResultAsync(context, terminalResult);
                    return;
                }

                if (parsedDirector is null || !parsedDirector.Ok || string.IsNullOrWhiteSpace(parsedDirector.Text))
                {
                    // Director can return HTTP 200 with ok:false; use 422 so turn logs and the client status match the error envelope.
                    finalStatusCode = 422;
                    await WriteForwardedResultAsync(
                        context,
                        new ForwardedModuleResult(422, terminalResult.ContentType, terminalResult.Body));
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

        internal static bool IsPersistTurnStep(EngineTurnPipelineStepInfo step)
        {
            return string.Equals(step.TargetModule, "session_store", StringComparison.OrdinalIgnoreCase)
                && string.Equals(EngineConfiguration.NormalizePath(step.Path), "/persist_turn", StringComparison.OrdinalIgnoreCase)
                && string.Equals(step.Method, "POST", StringComparison.OrdinalIgnoreCase);
        }

        internal static string RenderTurnPipelineStepBody(
            string template,
            int turn,
            string playerInput,
            ForwardedModuleResult? previousResult,
            IReadOnlyDictionary<string, ForwardedModuleResult> stepResults)
        {
            var rendered = new StringBuilder(template.Length + playerInput.Length);
            var cursor = 0;
            while (cursor < template.Length)
            {
                var open = template.IndexOf("{{", cursor, StringComparison.Ordinal);
                if (open < 0)
                {
                    rendered.Append(template, cursor, template.Length - cursor);
                    break;
                }

                var close = template.IndexOf("}}", open + 2, StringComparison.Ordinal);
                if (close < 0)
                {
                    throw new InvalidOperationException("Turn pipeline body_template contains an unterminated placeholder.");
                }

                rendered.Append(template, cursor, open - cursor);
                var placeholder = template[(open + 2)..close].Trim();
                rendered.Append(ResolveTurnPipelinePlaceholder(placeholder, turn, playerInput, previousResult, stepResults));
                cursor = close + 2;
            }

            var renderedJson = rendered.ToString();
            // Validate the rendered body before forwarding so configuration mistakes fail inside the router.
            using var _ = JsonDocument.Parse(renderedJson);
            return renderedJson;
        }

        internal static string ResolveTurnPipelinePlaceholder(
            string placeholder,
            int turn,
            string playerInput,
            ForwardedModuleResult? previousResult,
            IReadOnlyDictionary<string, ForwardedModuleResult> stepResults)
        {
            if (placeholder == "turn")
            {
                return turn.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }

            if (placeholder == "playerInputJson")
            {
                return JsonSerializer.Serialize(playerInput);
            }

            if (placeholder is "previous.rawBody" or "previous.rawBodyJson")
            {
                if (previousResult is null)
                {
                    throw new InvalidOperationException("Turn pipeline body_template referenced previous.rawBody before any step executed.");
                }

                return placeholder.EndsWith("Json", StringComparison.Ordinal)
                    ? JsonSerializer.Serialize(previousResult.Body)
                    : previousResult.Body;
            }

            const string stepPrefix = "step.";
            if (placeholder.StartsWith(stepPrefix, StringComparison.Ordinal))
            {
                var suffixStart = placeholder.IndexOf(".rawBody", stepPrefix.Length, StringComparison.Ordinal);
                if (suffixStart < 0)
                {
                    throw new InvalidOperationException($"Unsupported turn pipeline placeholder '{{{{{placeholder}}}}}'.");
                }

                var stepId = placeholder[stepPrefix.Length..suffixStart];
                if (string.IsNullOrWhiteSpace(stepId) || !stepResults.TryGetValue(stepId, out var stepResult))
                {
                    throw new InvalidOperationException($"Turn pipeline body_template referenced step '{stepId}' before it executed.");
                }

                var suffix = placeholder[suffixStart..];
                return suffix switch
                {
                    ".rawBody" => stepResult.Body,
                    ".rawBodyJson" => JsonSerializer.Serialize(stepResult.Body),
                    _ => throw new InvalidOperationException($"Unsupported turn pipeline placeholder '{{{{{placeholder}}}}}'.")
                };
            }

            throw new InvalidOperationException($"Unsupported turn pipeline placeholder '{{{{{placeholder}}}}}'.");
        }

        internal static string TruncateForLog(string text, int maxLen = 160)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            return text.Length <= maxLen ? text : text[..maxLen] + "…";
        }

        /// <summary>Same shape as LlmProvider_qwen log previews: long text keeps head and tail with an ellipsis gap in the middle.</summary>
        internal static string TruncateMiddle(string text, int headChars = 80, int tailChars = 60)
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
