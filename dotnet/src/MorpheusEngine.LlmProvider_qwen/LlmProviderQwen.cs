using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class LlmProviderQwen
    {
        #region Nested types
        private sealed class GenerateRequest
        {
            public string Prompt { get; set; } = ""; // Request specific, ex: classify this player input into classes of intent.
            public string Model { get; set; } = "";
            public string System { get; set; } = ""; // Used to pass general instructions that describe the role of the LLM. Ex: respond only with json
        }
        #endregion

        #region Public data
        public const int PORT = 8791;
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
                    await ProcessQuery(context);
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
            _listener.Prefixes.Add($"http://127.0.0.1:{PORT}/");
            _listener.Start();
            Console.WriteLine($"LlmProvider_qwen listening on http://127.0.0.1:{PORT}/");
            Console.WriteLine("LlmProvider_qwen initialized.");
        }

        private async Task ProcessQuery(HttpListenerContext context)
        {
            if (context.Request.Url is null)
            {
                Console.WriteLine("LlmProvider_qwen received invalid request: Url is null.");
                Respond(context, 400, new { ok = false, error = "Invalid request URL." });
                return;
            }

            var path = context.Request.Url.AbsolutePath;

            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
            {
                Console.WriteLine("LlmProvider_qwen/info called.");
                Respond(context, 200, new
                {
                    ok = true,
                    module_name = "llm_provider_qwen",
                    provider = "ollama",
                    model = DEFAULT_MODEL
                });
                return;
            }

            if (path.Equals("/generate", StringComparison.OrdinalIgnoreCase))
            {
                Console.WriteLine("LlmProvider_qwen/generate called.");
                await ProcessRequest_generate(context);
                return;
            }

            Console.WriteLine("LlmProvider_qwen called with an unknown path: " + path);
            Respond(context, 404, new { ok = false, error = "Not found: " + path });
        }

        public void RequestShutdown() => _shutdownRequested = true;

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();
            Console.WriteLine("LlmProvider_qwen shut down.");
        }

        private async Task ProcessRequest_generate(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            GenerateRequest? request = null;
            try
            {
                request = JsonSerializer.Deserialize<GenerateRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                Respond(context, 400, new { ok = false, error = "Invalid JSON payload.", details = e.Message });
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.Prompt))
            {
                Respond(context, 400, new { ok = false, error = "Request must include a non-empty 'prompt' field." });
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

            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync("http://127.0.0.1:11434/api/generate", content);
            }
            catch (Exception e)
            {
                Console.WriteLine("OLLAMA_IO ERROR Failed to reach Ollama: " + e.Message);
                Respond(context, 502, new
                {
                    ok = false,
                    error = "Failed to reach Ollama. Ensure Ollama is running locally on port 11434.",
                    details = e.Message
                });
                return;
            }

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            Console.WriteLine($"OLLAMA_IO RESPONSE status={(int)ollamaResponse.StatusCode} body={ollamaBody}");
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                Respond(context, (int)ollamaResponse.StatusCode, new
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

            Respond(context, 200, new
            {
                ok = true,
                model,
                response = responseText,
                ollama_raw = ollamaBody
            });
        }

        #region Helpers
        private void Respond(HttpListenerContext context, int statusCode, object payload)
        {
            var response = context.Response;
            response.StatusCode = statusCode;
            response.ContentType = "application/json";
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload));
            response.ContentLength64 = bytes.LongLength;
            response.OutputStream.Write(bytes);
            response.OutputStream.Close();
        }
        #endregion
    }
}
