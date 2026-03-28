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

        private static readonly HttpClient _httpClient = new HttpClient // Outbound http client for communicating with Ollama.
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        private readonly HttpListener _listener = new HttpListener(); // Inbound listener for responding to http messages.
        private bool _shutdownRequested = false;
        #endregion

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
                Console.WriteLine("LlmProvider_qwen error encountered: " + e.Message);
            }
            finally
            {
                Shutdown();
            }
        }

        private void Initialize()
        {
            var ports = EngineConfigLoader.GetPorts();
            _listener.Prefixes.Add($"http://127.0.0.1:{ports.LlmProviderQwen}/");
            _listener.Start();
            Console.WriteLine($"LlmProvider_qwen listening on http://127.0.0.1:{ports.LlmProviderQwen}/");
            Console.WriteLine("LlmProvider_qwen initialized.");
        }

        private async Task ProcessQuery(HttpListenerContext context)
        {
            try
            {
                if (context.Request.Url is null)
                {
                    Console.WriteLine("LlmProvider_qwen received invalid request: Url is null.");
                    await Respond(context, 400, new ErrorResponse(false, "Invalid request URL."));
                    return;
                }

                var path = context.Request.Url.AbsolutePath;

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

                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleHealthResponse(true, "llm_provider_qwen", "healthy"));
                    return;
                }

                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleShutdownResponse(true, "llm_provider_qwen", "Shutdown requested."));
                    BeginShutdown();
                    return;
                }

                if (path.Equals("/generate", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/generate called.");
                    await ProcessRequest_generate(context);
                    return;
                }

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

        public void RequestShutdown() => _shutdownRequested = true;

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();
            Console.WriteLine("LlmProvider_qwen shut down.");
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

        private async Task ProcessRequest_generate(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            QwenGenerateRequest? request = null;
            try
            {
                request = JsonSerializer.Deserialize<QwenGenerateRequest>(body, _jsonOptions);
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

            var model = string.IsNullOrWhiteSpace(request.Model) ? DEFAULT_MODEL : request.Model;

            var ollamaPayload = new
            {
                model,
                prompt = request.Prompt,
                system = request.System,
                stream = false // Generate whole response in one go and return it.
            };
            Console.WriteLine("OLLAMA_IO REQUEST " + JsonSerializer.Serialize(ollamaPayload));

            var content = new StringContent(
                JsonSerializer.Serialize(ollamaPayload),
                Encoding.UTF8,
                "application/json");

            var ollamaPort = EngineConfigLoader.GetPorts().Ollama;
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

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            Console.WriteLine($"OLLAMA_IO RESPONSE status={(int)ollamaResponse.StatusCode} body={ollamaBody}");
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new
                {
                    ok = false,
                    error = "Ollama returned an error.",
                    model,
                    ollama_status = (int)ollamaResponse.StatusCode,
                    ollama_response = ollamaBody
                });
                return;
            }

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

            await Respond(context, 200, new
            {
                ok = true,
                model,
                response = responseText,
                ollama_raw = ollamaBody
            });
        }

        #region Helpers
        private async Task Respond(HttpListenerContext context, int statusCode, object payload)
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
