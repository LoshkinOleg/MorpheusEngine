using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class Router
    {
        private static readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private readonly HttpListener _listener = new HttpListener();
        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
        private bool _shutdownRequested;
        private static readonly HttpClient _httpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        public void RequestShutdown() => _shutdownRequested = true;

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
                Console.WriteLine("Error encountered: " + e.Message);
            }
            finally
            {
                Shutdown();
            }
        }

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
                    BeginShutdown();
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

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();

            Console.WriteLine("Router shut down.");
        }

        private void BeginShutdown()
        {
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

            var result = await ForwardModuleCallAsync(
                "player_ui",
                "intent_extractor",
                "/intent",
                "POST",
                body);

            await WriteForwardedResultAsync(context, result);
        }

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

            var method = string.IsNullOrWhiteSpace(request.Method)
                ? "POST"
                : request.Method.Trim().ToUpperInvariant();
            if (method != "GET" && method != "POST")
            {
                await RespondAsync(context, 400, new ErrorResponse(false, $"Unsupported proxy method '{request.Method}'."));
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

        private async Task<string> ReadRequestBodyAsync(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            return await reader.ReadToEndAsync();
        }

        private async Task<ForwardedModuleResult> ForwardModuleCallAsync(
            string sourceModule,
            string targetModuleKey,
            string targetPath,
            string method,
            string? requestBody)
        {
            var normalizedPath = EngineConfiguration.NormalizePath(targetPath);
            var methodUpper = method.Trim().ToUpperInvariant();
            var targetModule = _configuration.FindModule(targetModuleKey);
            if (targetModule is null)
            {
                return ForwardedModuleResult.FromError(400, $"Unknown target module '{targetModuleKey}'.");
            }

            var targetPort = _configuration.ResolvePort(targetModule.PortKey);
            if (targetPort is null)
            {
                return ForwardedModuleResult.FromError(500, $"Target module '{targetModuleKey}' does not have a configured listening port.");
            }

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
                using var request = new HttpRequestMessage(new HttpMethod(methodUpper), uri);
                if (methodUpper == "POST")
                {
                    request.Content = string.IsNullOrWhiteSpace(requestBody)
                        ? new ByteArrayContent(Array.Empty<byte>())
                        : new StringContent(requestBody, Encoding.UTF8, "application/json");
                }

                using var response = await _httpClient.SendAsync(request);
                var responseBody = await response.Content.ReadAsStringAsync();
                var contentType = response.Content.Headers.ContentType?.MediaType ?? "application/json";
                Console.WriteLine($"[RouterProxy] {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath} => {(int)response.StatusCode}");
                return new ForwardedModuleResult(
                    (int)response.StatusCode,
                    contentType,
                    responseBody);
            }
            catch (Exception e)
            {
                Console.WriteLine($"[RouterProxy] {sourceModule} -> {targetModule.PortKey} {methodUpper} {normalizedPath} => network error: {e.Message}");
                return ForwardedModuleResult.FromError(
                    502,
                    $"Failed to reach target module '{targetModule.PortKey}'.",
                    e.Message);
            }
        }

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

        private sealed record ForwardedModuleResult(int StatusCode, string ContentType, string Body)
        {
            public static ForwardedModuleResult FromError(int statusCode, string error, string? details = null) =>
                new(
                    statusCode,
                    "application/json",
                    JsonSerializer.Serialize(new ErrorResponse(false, error, details)));
        }
    }
}