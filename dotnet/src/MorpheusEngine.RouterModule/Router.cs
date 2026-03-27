using System.Net;
using System.Text.Json;
using System.Text;

namespace MorpheusEngine
{
    public class Router
    {
        private sealed class QwenGenerateRequest
        {
            public string Prompt { get; set; } = string.Empty;
        }

        private sealed class TurnRequest
        {
            public string PlayerInput { get; set; } = string.Empty;
        }

        public const int PORT = 8790;
        public const int QWEN_PORT = 8791;

        private HttpListener _listener = new HttpListener();
        private bool _shutdownRequested = false;
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
                    HttpListenerContext context = await _listener.GetContextAsync();
                    ProcessQuery(context);
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
            _listener.Prefixes.Add($"http://127.0.0.1:{PORT}/");
            _listener.Start();
            Console.WriteLine($"Router module listening on http://127.0.0.1:{PORT}/");

            Console.WriteLine("Router initialized.");
        }
        private void ProcessQuery(HttpListenerContext context)
        {
            if (context.Request.Url == null)
            {
                return;
            }

            var path = context.Request.Url.AbsolutePath;

            Console.WriteLine("Router received call: " + path);

            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
            {
                ProcessRequest_info(context);
                return;
            }

            if (path.Equals("/turn", StringComparison.OrdinalIgnoreCase))
            {
                ProcessRequest_turn(context);
                return;
            }

            Console.WriteLine("Request for router did not match any expected endpoints. Returning 404.");
            Respond(context, 404, new { ok = false, error = "Not found: " + path});
        }
        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();

            Console.WriteLine("Router shut down.");
        }

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

        private void ProcessRequest_info(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            var body = reader.ReadToEnd();

            Console.WriteLine("Router.ProcessRequest_info(): responding with 200.");
            Respond(context, 200, new { ok = true, module_name = "router" });
        }

        private void ProcessRequest_turn(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            var body = reader.ReadToEnd();

            TurnRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<TurnRequest>(body, new JsonSerializerOptions
                {
                    PropertyNameCaseInsensitive = true
                });
            }
            catch (JsonException e)
            {
                Respond(context, 400, new { ok = false, error = "Invalid JSON payload.", details = e.Message });
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
            {
                Respond(context, 400, new { ok = false, error = "Request must include a non-empty 'playerInput' field." });
                return;
            }

            // Request is valid, proceed.

            var qwenPayload = new QwenGenerateRequest
            {
                Prompt = request.PlayerInput
            };

            var content = new StringContent(
                JsonSerializer.Serialize(qwenPayload),
                Encoding.UTF8,
                "application/json");

            HttpResponseMessage qwenResponse;
            try
            {
                qwenResponse = _httpClient.PostAsync(
                    $"http://127.0.0.1:{QWEN_PORT}/generate",
                    content).GetAwaiter().GetResult();
            }
            catch (Exception e)
            {
                Respond(context, 502, new
                {
                    ok = false,
                    error = "Failed to reach LlmProvider_qwen.",
                    details = e.Message
                });
                return;
            }

            var qwenBody = qwenResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult();
            if (!qwenResponse.IsSuccessStatusCode)
            {
                Respond(context, (int)qwenResponse.StatusCode, new
                {
                    ok = false,
                    error = "LlmProvider_qwen returned an error.",
                    qwen_status = (int)qwenResponse.StatusCode,
                    qwen_response = qwenBody
                });
                return;
            }

            Respond(context, 200, new
            {
                ok = true,
                endpoint = "turn",
                player_input = request.PlayerInput,
                qwen_response = JsonSerializer.Deserialize<JsonElement>(qwenBody)
            });
        }
    }
}