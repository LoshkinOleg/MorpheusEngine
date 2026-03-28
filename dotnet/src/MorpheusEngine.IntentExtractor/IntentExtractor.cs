using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class IntentExtractor
    {
        private readonly HttpListener _listener = new HttpListener();
    private static readonly HttpClient _httpClient = new()
    {
        Timeout = TimeSpan.FromSeconds(30)
    };
        private static readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };
    private const string IntentExtractionSystemPrompt =
        "You extract intents for a text adventure engine. " +
        "Return JSON only with this exact shape: " +
        "{\"intent\":\"string\",\"params\":{\"key\":\"value\"}}. " +
        "Use one of these intents when possible: inspect, move_self, wait, attack, take, talk, freeform_action. " +
        "Use params.target for the main subject when relevant. " +
        "If the input does not fit a known verb, return freeform_action and include params.text.";
        private bool _shutdownRequested;

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
                Console.WriteLine("IntentExtractor error encountered: " + e.Message);
            }
            finally
            {
                Shutdown();
            }
        }

        public void RequestShutdown() => _shutdownRequested = true;

        private void Initialize()
        {
            var ports = EngineConfigLoader.GetPorts();
            _listener.Prefixes.Add($"http://127.0.0.1:{ports.IntentExtractor}/");
            _listener.Start();
            Console.WriteLine($"IntentExtractor listening on http://127.0.0.1:{ports.IntentExtractor}/");
            Console.WriteLine("IntentExtractor initialized.");
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
                    await Respond(context, 200, new ModuleInfoResponse(true, "intent_extractor"));
                    return;
                }

                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleHealthResponse(true, "intent_extractor", "healthy"));
                    return;
                }

                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleShutdownResponse(true, "intent_extractor", "Shutdown requested."));
                    BeginShutdown();
                    return;
                }

                if (path.Equals("/intent", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_intent(context);
                    return;
                }

                await Respond(context, 404, new ErrorResponse(false, "Not found: " + path));
            }
            catch (Exception e)
            {
                Console.WriteLine("IntentExtractor encountered unhandled request error: " + e.Message);
                if (context.Response.OutputStream.CanWrite)
                {
                    await Respond(context, 500, new ErrorResponse(false, "Unhandled intent extractor error.", e.Message));
                }
            }
        }

        private async Task ProcessRequest_intent(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            IntentRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<IntentRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include a non-empty 'playerInput' field."));
                return;
            }

            var extraction = await ExtractIntentAsync(request.PlayerInput);
            await Respond(context, 200, new IntentResponse(true, extraction.Intent, extraction.Parameters));
        }

        private async Task<IntentExtractionResult> ExtractIntentAsync(string playerInput)
        {
            try
            {
                var qwenRequest = new QwenGenerateRequest(
                    BuildIntentPrompt(playerInput),
                    "qwen2.5:7b-instruct",
                    IntentExtractionSystemPrompt);

                var proxyRequest = new ModuleProxyRequest(
                    "intent_extractor",
                    "llm_provider_qwen",
                    "/generate",
                    "POST",
                    JsonSerializer.SerializeToElement(qwenRequest));

                using var content = new StringContent(
                    JsonSerializer.Serialize(proxyRequest),
                    Encoding.UTF8,
                    "application/json");

                var ports = EngineConfigLoader.GetPorts();
                using var qwenResponse = await _httpClient.PostAsync(
                    $"http://127.0.0.1:{ports.Router}/proxy",
                    content);

                var qwenBody = await qwenResponse.Content.ReadAsStringAsync();
                if (!qwenResponse.IsSuccessStatusCode)
                {
                    Console.WriteLine($"IntentExtractor could not use router proxy to reach LlmProvider_qwen. Status={(int)qwenResponse.StatusCode}. Falling back to heuristic extraction.");
                    return ExtractIntentHeuristic(playerInput);
                }

                QwenGenerateResponse? payload;
                try
                {
                    payload = JsonSerializer.Deserialize<QwenGenerateResponse>(qwenBody, _jsonOptions);
                }
                catch (JsonException)
                {
                    Console.WriteLine("IntentExtractor received invalid JSON from the router-proxied LlmProvider_qwen response. Falling back to heuristic extraction.");
                    return ExtractIntentHeuristic(playerInput);
                }

                if (string.IsNullOrWhiteSpace(payload?.Response))
                {
                    Console.WriteLine("IntentExtractor received an empty response from router-proxied LlmProvider_qwen. Falling back to heuristic extraction.");
                    return ExtractIntentHeuristic(playerInput);
                }

                if (TryParseIntentResult(payload.Response, out var extraction))
                {
                    return extraction;
                }

                Console.WriteLine("IntentExtractor could not parse LlmProvider_qwen output as intent JSON. Falling back to heuristic extraction.");
            }
            catch (Exception e)
            {
                Console.WriteLine("IntentExtractor failed to reach LlmProvider_qwen through the router proxy. Falling back to heuristic extraction. " + e.Message);
            }

            return ExtractIntentHeuristic(playerInput);
        }

        private static string BuildIntentPrompt(string playerInput) =>
            "Player input:\n" +
            playerInput.Trim() +
            "\n\nReturn only JSON.";

        private static bool TryParseIntentResult(string rawResponse, out IntentExtractionResult extraction)
        {
            extraction = default!;

            var candidate = ExtractJsonObject(rawResponse);
            if (candidate is null)
            {
                return false;
            }

            try
            {
                using var doc = JsonDocument.Parse(candidate);
                var root = doc.RootElement;
                if (!root.TryGetProperty("intent", out var intentElement))
                {
                    return false;
                }

                var intent = intentElement.GetString();
                if (string.IsNullOrWhiteSpace(intent))
                {
                    return false;
                }

                var parameters = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                if (root.TryGetProperty("params", out var paramsElement))
                {
                    CopyStringMap(paramsElement, parameters);
                }
                else if (root.TryGetProperty("parameters", out var parametersElement))
                {
                    CopyStringMap(parametersElement, parameters);
                }

                extraction = new IntentExtractionResult(intent.Trim(), parameters);
                return true;
            }
            catch (JsonException)
            {
                return false;
            }
        }

        private static string? ExtractJsonObject(string rawResponse)
        {
            var trimmed = rawResponse.Trim();
            if (trimmed.StartsWith("```", StringComparison.Ordinal))
            {
                var firstNewline = trimmed.IndexOf('\n');
                if (firstNewline >= 0)
                {
                    trimmed = trimmed[(firstNewline + 1)..];
                }

                var closingFence = trimmed.LastIndexOf("```", StringComparison.Ordinal);
                if (closingFence >= 0)
                {
                    trimmed = trimmed[..closingFence];
                }

                trimmed = trimmed.Trim();
            }

            var start = trimmed.IndexOf('{');
            var end = trimmed.LastIndexOf('}');
            if (start < 0 || end <= start)
            {
                return null;
            }

            return trimmed.Substring(start, end - start + 1);
        }

        private static void CopyStringMap(JsonElement element, Dictionary<string, string> destination)
        {
            if (element.ValueKind != JsonValueKind.Object)
            {
                return;
            }

            foreach (var property in element.EnumerateObject())
            {
                destination[property.Name] = property.Value.ValueKind switch
                {
                    JsonValueKind.String => property.Value.GetString() ?? string.Empty,
                    JsonValueKind.Number => property.Value.ToString(),
                    JsonValueKind.True => bool.TrueString,
                    JsonValueKind.False => bool.FalseString,
                    JsonValueKind.Null => string.Empty,
                    _ => property.Value.GetRawText()
                };
            }
        }

        private static IntentExtractionResult ExtractIntentHeuristic(string playerInput)
        {
            var normalizedInput = playerInput.Trim();
            var lowered = normalizedInput.ToLowerInvariant();
            var words = normalizedInput.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            var primaryVerb = words.Length > 0 ? words[0].ToLowerInvariant() : string.Empty;
            var remainingText = words.Length > 1 ? string.Join(' ', words.Skip(1)) : string.Empty;

            return primaryVerb switch
            {
                "look" or "inspect" or "examine" => BuildResult("inspect", remainingText),
                "go" or "walk" or "move" or "run" => BuildResult("move_self", remainingText),
                "wait" or "rest" or "pause" => new IntentExtractionResult("wait", new Dictionary<string, string>()),
                "attack" or "hit" or "strike" => BuildResult("attack", remainingText),
                "take" or "grab" or "pick" => BuildResult("take", remainingText),
                "talk" or "speak" or "ask" => BuildResult("talk", remainingText),
                _ => ExtractIntentByPhrase(lowered, normalizedInput)
            };
        }

        private static IntentExtractionResult ExtractIntentByPhrase(string loweredInput, string originalInput)
        {
            if (loweredInput.Contains("look") || loweredInput.Contains("inspect") || loweredInput.Contains("examine"))
            {
                return BuildResult("inspect", originalInput);
            }

            if (loweredInput.Contains("go ") || loweredInput.Contains("walk ") || loweredInput.Contains("move "))
            {
                return BuildResult("move_self", originalInput);
            }

            if (loweredInput.Contains("attack") || loweredInput.Contains("hit ") || loweredInput.Contains("strike "))
            {
                return BuildResult("attack", originalInput);
            }

            if (loweredInput.Contains("talk") || loweredInput.Contains("speak") || loweredInput.Contains("ask "))
            {
                return BuildResult("talk", originalInput);
            }

            return new IntentExtractionResult("freeform_action", new Dictionary<string, string>
            {
                ["text"] = originalInput
            });
        }

        private static IntentExtractionResult BuildResult(string intent, string targetText)
        {
            var parameters = new Dictionary<string, string>();
            if (!string.IsNullOrWhiteSpace(targetText))
            {
                parameters["target"] = targetText.Trim();
            }

            return new IntentExtractionResult(intent, parameters);
        }

        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();
            Console.WriteLine("IntentExtractor shut down.");
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

        private sealed record IntentExtractionResult(string Intent, IReadOnlyDictionary<string, string> Parameters);
        private sealed record QwenGenerateResponse(bool Ok, string? Model, string? Response, string? OllamaRaw);
    }
}
