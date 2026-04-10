using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class LlmProviderQwen
    {
        #region Public data
        public const string DEFAULT_MODEL = "qwen2.5:7b-instruct";
        #endregion

        #region Private data
        private static readonly JsonSerializerOptions _jsonOptions = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true // Allows either casing for json fields.
        };

        // Instance-owned HttpClient: we dispose it in Shutdown() after the listener stops so sockets are released cleanly for this process.
        private readonly HttpClient _httpClient = new()
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        private readonly HttpListener _listener = new HttpListener(); // Inbound listener for responding to http messages.
        private bool _shutdownRequested = false;
        #endregion

        #region Public methods
        public async Task Run()
        {
            // Bind and start listening before entering the accept loop.
            Initialize();

            try
            {
                // Block until a request arrives, then handle it without awaiting (concurrent requests).
                while (!_shutdownRequested)
                {
                    var context = await _listener.GetContextAsync();
                    _ = ProcessQuery(context);
                }
            }
            catch (HttpListenerException e)
            {
                Console.WriteLine("LlmProvider_qwen error encountered: " + e.Message);
            }
            finally
            {
                // Always release listener and outbound HTTP resources when the loop ends or faults.
                Shutdown();
            }
        }

        public void RequestShutdown() => _shutdownRequested = true;
        #endregion

        #region Private methods
        // Intentional single use method.
        private void Initialize()
        {
            var ports = EngineConfigLoader.GetPorts();
            var qwenListen = ports.GetRequiredPort("llm_provider_qwen");
            _listener.Prefixes.Add($"http://127.0.0.1:{qwenListen}/");
            _listener.Start();
            Console.WriteLine($"LlmProvider_qwen listening on http://127.0.0.1:{qwenListen}/");
            Console.WriteLine("LlmProvider_qwen initialized.");
        }

        // Intentional single use method.
        private async Task ProcessQuery(HttpListenerContext context)
        {
            try
            {
                // Contract checks.
                if (context.Request.Url is null)
                {
                    Console.WriteLine("LlmProvider_qwen received invalid request: Url is null.");
                    await Respond(context, 400, new ErrorResponse(false, "Invalid request URL."));
                    return;
                }

                var path = context.Request.Url.AbsolutePath;

                // /info endpoint.
                if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/info called.");
                    await Respond(context, 200, new
                    {
                        ok = true,
                        module_name = "llm_provider_qwen",
                        provider = "ollama",
                        model = DEFAULT_MODEL
                    });
                    return;
                }

                // /health endpoint
                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleHealthResponse(true, "llm_provider_qwen", "healthy"));
                    return;
                }

                // /shutdown endpoint
                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_shutdown(context);
                    return;
                }

                // /generate endpoint.
                if (path.Equals("/generate", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/generate called.");
                    await ProcessRequest_generate(context);
                    return;
                }

                // Invalid endpoint specified.
                Console.WriteLine("LlmProvider_qwen called with an unknown path: " + path);
                await Respond(context, 404, new ErrorResponse(false, "Not found: " + path));
            }
            catch (Exception e)
            {
                Console.WriteLine("LlmProvider_qwen encountered unhandled request error: " + e.Message);
                if (context.Response.OutputStream.CanWrite)
                {
                    await Respond(context, 500, new ErrorResponse(false, "Unhandled llm provider error.", e.Message));
                }
            }
        }

        // Intentional single use method.
        private void Shutdown()
        {
            _listener.Stop(); // Technically redundant as it's included in _listener.Close().
            _listener.Close();
            _httpClient.Dispose();
            Console.WriteLine("LlmProvider_qwen shut down.");
        }

        // Exception to "extract only when >1 use": kept as a named handler parallel to ProcessRequest_generate for /shutdown routing clarity.
        private async Task ProcessRequest_shutdown(HttpListenerContext context)
        {
            await Respond(context, 200, new ModuleShutdownResponse(true, "llm_provider_qwen", "Shutdown requested."));
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

        // Intentional single use method.
        private async Task ProcessRequest_generate(HttpListenerContext context)
        {
            // Parse caller's request.
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            LlmGenerateRequest? request = null;
            try
            {
                request = JsonSerializer.Deserialize<LlmGenerateRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.Prompt))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include a non-empty 'prompt' field."));
                return;
            }
            // Request validated.

            // No default model: caller must supply model explicitly (fail fast).
            if (string.IsNullOrWhiteSpace(request.Model))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include a non-empty 'model' field."));
                return;
            }

            var model = request.Model.Trim();

            // Construct an ollama payload from the internal generic payload.
            var ollamaPayload = new
            {
                model,
                prompt = request.Prompt,
                system = request.System,
                stream = false // Generate whole response in one go and return it.
            };
            Console.WriteLine("OLLAMA_IO REQUEST " + JsonSerializer.Serialize(ollamaPayload));

            // Convert to json for transmission.
            var content = new StringContent(
                JsonSerializer.Serialize(ollamaPayload),
                Encoding.UTF8,
                "application/json");

            // Send message to ollama.
            var ollamaPort = EngineConfigLoader.GetConfiguration().LlmProviderOllamaListenPort;
            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync($"http://127.0.0.1:{ollamaPort}/api/generate", content);
            }
            catch (Exception e)
            {
                Console.WriteLine("OLLAMA_IO ERROR Failed to reach Ollama: " + e.Message);
                await Respond(context, 502, new
                {
                    ok = false,
                    error = $"Failed to reach Ollama. Ensure Ollama is running locally on port {ollamaPort}.",
                    details = e.Message
                });
                return;
            }
            // Received response from ollama / errored out.

            // Relay the ollama response back to caller.
            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            Console.WriteLine($"OLLAMA_IO RESPONSE status={(int)ollamaResponse.StatusCode} body={ollamaBody}");
            if (!ollamaResponse.IsSuccessStatusCode) // Ollama errored out, relay message to caller.
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new
                {
                    ok = false,
                    error = "Ollama returned an error.",
                    model,
                    ollama_status = (int)ollamaResponse.StatusCode,
                    ollama_response = ollamaBody
                });
                return; // End of method execution if ollama errored out.
            }

            // Call to ollama was successful.
            // Parse ollama response.
            string? responseText = null;
            try
            {
                using var doc = JsonDocument.Parse(ollamaBody);
                if (doc.RootElement.TryGetProperty("response", out var responseElement))
                {
                    responseText = responseElement.GetString();
                }
            }
            catch (JsonException)
            {
                // keep raw body when parsing fails
            }

            // Relay the successful ollama response back to the caller.
            await Respond(context, 200, new LlmProviderGenerateResponse(true, model, responseText, ollamaBody));
        }
        #endregion

        #region Helper methods
        private async Task Respond(HttpListenerContext context, int statusCode, object payload)
        {
            var response = context.Response;
            response.StatusCode = statusCode;
            response.ContentType = "application/json";
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload)); // using the "object" type to avoid having to define a type for every kind of communication.
            response.ContentLength64 = bytes.LongLength;
            await response.OutputStream.WriteAsync(bytes);
            response.OutputStream.Close();
        }
        #endregion
    }
}
