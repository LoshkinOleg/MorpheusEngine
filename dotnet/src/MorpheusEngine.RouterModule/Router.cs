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

        /// <summary>Set when <c>/shutdown</c> is received or <see cref="RequestShutdown"/> is called; exits the accept loop.</summary>
        private bool _shutdownRequested;

        private static readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private static readonly HttpClient _httpClient = new()
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        /// <summary>
        /// Proxied responses from downstream modules must declare this media type (no silent fallback to JSON).
        /// </summary>
        private const string ExpectedProxiedResponseMediaType = "application/json";

        #endregion

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
            _listener.Prefixes.Add($"http://127.0.0.1:{_configuration.Ports.Router}/");
            _listener.Start();
            Console.WriteLine($"Router module listening on http://127.0.0.1:{_configuration.Ports.Router}/");
            Console.WriteLine("Router initialized.");
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
                Console.WriteLine("Router received call: " + path);

                if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
                {
                    await RespondAsync(context, 200, new ModuleInfoResponse(true, "router"));
                    return;
                }

                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    await RespondAsync(context, 200, new ModuleHealthResponse(true, "router", "healthy"));
                    return;
                }

                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await RespondAsync(context, 200, new ModuleShutdownResponse(true, "router", "Shutdown requested."));
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

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();
            Console.WriteLine("Router shut down.");
        }

        /// <summary>
        /// Player turn: validate body as <see cref="TurnRequest"/>, then forward the same JSON body to intent extractor <c>/intent</c>.
        /// Today <c>TurnRequest</c> and <c>IntentRequest</c> share the <c>playerInput</c> shape; later a dedicated mapping may be needed.
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

            if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
            {
                await RespondAsync(context, 400, new ErrorResponse(false, "Request must include a non-empty 'playerInput' field."));
                return;
            }

            // Audit label for logs only; not authenticated. Same mechanism as /proxy uses for cross-module calls.
            var result = await ForwardModuleCallAsync(
                "player_ui",
                "intent_extractor",
                "/intent",
                "POST",
                body);

            await WriteForwardedResultAsync(context, result);
        }

        /// <summary>
        /// Generic proxy: caller supplies target module key, path, HTTP method, and optional JSON body.
        /// Only pairs (path, method) that appear on that module in <c>engine_config.json</c> are allowed.
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
                await RespondAsync(context, 400, new ErrorResponse(false, "Proxy request must include source_module, target_module, and target_path."));
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
        /// <param name="sourceModule">Label for audit logs only (e.g. <c>intent_extractor</c>, <c>player_ui</c>).</param>
        /// <param name="targetModuleKey"><c>port_key</c> from config, e.g. <c>llm_provider_qwen</c>.</param>
        /// <param name="targetPath">Path on the target, e.g. <c>/generate</c>.</param>
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

            var targetPort = _configuration.ResolvePort(targetModule.PortKey);
            if (targetPort is null)
            {
                return ForwardedModuleResult.FromError(500, $"Target module '{targetModuleKey}' does not have a configured listening port.");
            }

            // Allowlist: only (path, method) pairs declared on this module in engine_config.json may be reached through the proxy.
            // This blocks arbitrary SSRF-style forwarding even though the caller is on localhost.
            var endpoint = targetModule.Endpoints.FirstOrDefault(ep =>
                string.Equals(EngineConfiguration.NormalizePath(ep.Path), normalizedPath, StringComparison.OrdinalIgnoreCase)
                && string.Equals(ep.Method, methodUpper, StringComparison.OrdinalIgnoreCase));
            if (endpoint is null)
            {
                return ForwardedModuleResult.FromError(403, $"Proxy target '{targetModuleKey} {methodUpper} {normalizedPath}' is not allowed by configuration.");
            }

            var uri = $"http://127.0.0.1:{targetPort.Value}{normalizedPath}";
            Console.WriteLine($"[RouterProxy] {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath}");

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

                Console.WriteLine($"[RouterProxy] {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath} => {(int)response.StatusCode}");

                // What the router's caller receives: same status code, content type, and body the router got from the target module.
                return new ForwardedModuleResult(
                    (int)response.StatusCode,
                    ExpectedProxiedResponseMediaType,
                    responseBody);
            }
            catch (Exception e) when (e is not InvalidOperationException)
            {
                Console.WriteLine($"[RouterProxy] {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath} => network error: {e.Message}");
                return ForwardedModuleResult.FromError(
                    502,
                    $"Failed to reach target module '{targetModule.PortKey}'.",
                    e.Message);
            }
        }

        #region Helpers

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
        /// Router-native JSON responses: serializes a CLR object to JSON and always sets <c>application/json</c>.
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
