using System.Diagnostics.CodeAnalysis;
using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class IntentExtractor
    {
        #region Nested types

        // Structured intent plus string parameters after JSON parse (before or after catalog validation).
        private sealed record IntentExtractionResult(string Intent, IReadOnlyDictionary<string, string> Parameters);

        #endregion

        #region Private data

        // Intent catalog and prompt: single source of truth so validation cannot drift from the system prompt.
        // TODO: at some point we need to make this data driven so that each game_project can define their own actions.
        private static readonly string[] AllowedIntents =
        [
            "inspect", "move_self", "wait", "attack", "take", "talk", "freeform_action"
        ];

        // Resolved once: ports, module list, intent default model, and router proxy aliases (e.g. generic_llm_provider -> llm_provider_qwen).
        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();

        // Instance-owned HttpClient: we dispose it in Shutdown() after the listener stops so sockets are released cleanly for this process.
        private readonly HttpClient _httpClient = new()
        {
            Timeout = TimeSpan.FromSeconds(30)
        };

        private readonly JsonSerializerOptions _jsonOptions = new()
        {
            PropertyNameCaseInsensitive = true // Allows either casing for json fields.
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
                Console.WriteLine("IntentExtractor error encountered: " + e.Message);
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
            var intentPort = ports.GetRequiredPort("intent_extractor");
            _listener.Prefixes.Add($"http://127.0.0.1:{intentPort}/");
            _listener.Start();
            Console.WriteLine($"IntentExtractor listening on http://127.0.0.1:{intentPort}/");
            Console.WriteLine("IntentExtractor initialized.");
        }

        // Intentional single use method.
        private void Shutdown()
        {
            _listener.Stop(); // Technically redundant as it's included in _listener.Close().
            _listener.Close();
            _httpClient.Dispose();
            Console.WriteLine("IntentExtractor shut down.");
        }

        // Intentional single use method.
        private async Task ProcessQuery(HttpListenerContext context)
        {
            try
            {
                // Contract checks.
                if (context.Request.Url is null)
                {
                    await Respond(context, 400, new ErrorResponse(false, "Invalid request URL."));
                    return;
                }

                var path = context.Request.Url.AbsolutePath;

                // /info endpoint.
                if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleInfoResponse(true, "intent_extractor"));
                    return;
                }

                // /health endpoint.
                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    await Respond(context, 200, new ModuleHealthResponse(true, "intent_extractor", "healthy"));
                    return;
                }

                // /shutdown endpoint.
                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_shutdown(context);
                    return;
                }

                // /intent endpoint.
                if (path.Equals("/intent", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_intent(context);
                    return;
                }

                // Invalid endpoint specified.
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

        // Intentional single use method. No heuristic fallback: LLM path failures become HTTP errors (fail fast).
        private async Task ProcessRequest_intent(HttpListenerContext context)
        {
            string BuildIntentPrompt(string playerInput)
            {
                return
                "Player input:\n" +
                playerInput.Trim() +
                "\n\nReturn only JSON.";
            }

            // Parse caller's request body.
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
            // IntentRequest validated.

            // Model comes from engine_config (intent_extraction.default_llm_model), not from this module.
            var model = _configuration.IntentDefaultLlmModel;

            // Assemble the generic LLM payload; router resolves generic_llm_provider to the configured provider module.
            var qwenRequest = new LlmGenerateRequest(
                BuildIntentPrompt(request.PlayerInput),
                model,
                BuildIntentSystemPrompt());

            var proxyRequest = new ModuleProxyRequest(
                "intent_extractor",
                "generic_llm_provider",
                "/generate",
                "POST",
                JsonSerializer.SerializeToElement(qwenRequest));

            // Convert to json for transmission to the router's /proxy endpoint.
            using var content = new StringContent(
                JsonSerializer.Serialize(proxyRequest, _jsonOptions),
                Encoding.UTF8,
                "application/json");

            var ports = EngineConfigLoader.GetPorts();
            HttpResponseMessage qwenResponse;
            try
            {
                qwenResponse = await _httpClient.PostAsync(
                    $"http://127.0.0.1:{ports.GetRequiredPort("router")}/proxy",
                    content);
            }
            catch (Exception e)
            {
                Console.WriteLine("[IntentExtractor] Router proxy request failed: " + e.Message);
                await Respond(context, 502, new ErrorResponse(false, "Failed to reach router proxy for LLM.", e.Message));
                return;
            }

            using (qwenResponse)
            {
                // Unwrap the router's response body (same JSON the LLM provider returned).
                var qwenBody = await qwenResponse.Content.ReadAsStringAsync();
                if (!qwenResponse.IsSuccessStatusCode)
                {
                    Console.WriteLine(
                        $"[IntentExtractor] Router proxy returned {(int)qwenResponse.StatusCode}: {qwenBody}");
                    await Respond(
                        context,
                        502,
                        new ErrorResponse(
                            false,
                            "Router proxy did not return success for LLM call.",
                            TruncateDetails(qwenBody)));
                    return;
                }

                LlmProviderGenerateResponse? payload;
                try
                {
                    payload = JsonSerializer.Deserialize<LlmProviderGenerateResponse>(qwenBody, _jsonOptions);
                }
                catch (JsonException e)
                {
                    Console.WriteLine("[IntentExtractor] Invalid JSON from proxied LLM provider: " + e.Message);
                    await Respond(
                        context,
                        422,
                        new ErrorResponse(false, "Proxied LLM response was not valid JSON.", e.Message));
                    return;
                }

                if (payload is null || string.IsNullOrWhiteSpace(payload.Response))
                {
                    Console.WriteLine("[IntentExtractor] Proxied LLM response missing 'response' text.");
                    await Respond(
                        context,
                        422,
                        new ErrorResponse(
                            false,
                            "LLM response was empty or missing 'response'.",
                            TruncateDetails(qwenBody)));
                    return;
                }

                // Parse the model's text field as strict intent JSON (params, not parameters).
                if (!TryParseIntentResult(payload.Response, out var extraction))
                {
                    await Respond(
                        context,
                        422,
                        new ErrorResponse(
                            false,
                            "Could not parse LLM output as intent JSON.",
                            TruncateDetails(payload.Response)));
                    return;
                }

                // Catalog rules: required target/text per intent.
                if (!TryNormalizeAndValidateIntent(extraction, out var validated, out var validationError))
                {
                    Console.WriteLine("[IntentExtractor] Intent validation failed: " + validationError);
                    await Respond(context, 422, new ErrorResponse(false, validationError ?? "Intent validation failed."));
                    return;
                }

                await Respond(
                    context,
                    200,
                    new IntentResponse(true, validated.Intent, validated.Parameters));
            }
        }

        // Exception to "extract only when >1 use": kept as a named handler parallel to ProcessRequest_intent for /shutdown routing clarity.
        private async Task ProcessRequest_shutdown(HttpListenerContext context)
        {
            await Respond(context, 200, new ModuleShutdownResponse(true, "intent_extractor", "Shutdown requested."));
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

        // Extracts intent + params from the LLM text; rejects "parameters" alias and non-object params.
        private static bool TryParseIntentResult(
            string rawResponse,
            [NotNullWhen(true)] out IntentExtractionResult? extraction)
        {
            extraction = null;

            var candidate = ExtractJsonObject(rawResponse);
            if (candidate is null)
            {
                Console.WriteLine("[IntentExtractor] Contract violation: could not find a JSON object in LLM output.");
                return false;
            }

            try
            {
                using var doc = JsonDocument.Parse(candidate);
                var root = doc.RootElement;

                if (root.TryGetProperty("parameters", out _))
                {
                    Console.WriteLine("[IntentExtractor] Contract violation: JSON must use 'params', not 'parameters'.");
                    return false;
                }

                if (!root.TryGetProperty("intent", out var intentElement))
                {
                    Console.WriteLine("[IntentExtractor] Contract violation: missing 'intent' property.");
                    return false;
                }

                var intent = intentElement.GetString();
                if (string.IsNullOrWhiteSpace(intent))
                {
                    Console.WriteLine("[IntentExtractor] Contract violation: 'intent' is empty.");
                    return false;
                }

                var parameters = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                if (root.TryGetProperty("params", out var paramsElement))
                {
                    if (paramsElement.ValueKind != JsonValueKind.Object)
                    {
                        Console.WriteLine("[IntentExtractor] Contract violation: 'params' must be a JSON object.");
                        return false;
                    }

                    CopyStringMap(paramsElement, parameters);
                }

                extraction = new IntentExtractionResult(intent.Trim(), parameters);
                return true;
            }
            catch (JsonException e)
            {
                Console.WriteLine("[IntentExtractor] Contract violation: failed to parse intent JSON: " + e.Message);
                return false;
            }
        }

        #endregion

        #region Helper methods

        // Static helpers: parsing and catalog logic are split out so ProcessRequest_intent stays readable (CodingStyle: avoid extra private methods unless >1 use or documented exception).

        private static string BuildIntentSystemPrompt()
        {
            var list = string.Join(", ", AllowedIntents);
            return
                "You extract player intents from player input for a text adventure game engine. " +
                "Return JSON only with this exact shape: {\"intent\":\"string\",\"params\":{\"key\":\"value\"}}. " +
                "Use the property name params. " +
                "Allowed intent values: " + list + ". " +
                "For inspect, move_self, attack, take, and talk, include a non-empty params.target for the main subject. " + // TODO: modify this at some point when the actions are made data driven.
                "For wait, params may be empty. " +
                "For freeform_action, include a non-empty params.text. " +
                "If the input does not fit a known verb, use freeform_action with params.text.";
        }

        // Maps LLM output to catalog intent names and enforces params.target / params.text rules.
        private static bool TryNormalizeAndValidateIntent(
            IntentExtractionResult parsed,
            [NotNullWhen(true)] out IntentExtractionResult? valid,
            out string? error)
        {
            valid = null;
            error = null;

            var canonical = NormalizeIntentName(parsed.Intent);
            if (canonical is null)
            {
                error = $"Unknown intent '{parsed.Intent}'. Allowed: {string.Join(", ", AllowedIntents)}.";
                return false;
            }

            if (RequiresTarget(canonical))
            {
                if (!parsed.Parameters.TryGetValue("target", out var t) || string.IsNullOrWhiteSpace(t))
                {
                    error = $"Intent '{canonical}' requires a non-empty params.target.";
                    return false;
                }
            }

            if (RequiresText(canonical))
            {
                if (!parsed.Parameters.TryGetValue("text", out var txt) || string.IsNullOrWhiteSpace(txt))
                {
                    error = $"Intent '{canonical}' requires a non-empty params.text.";
                    return false;
                }
            }

            valid = new IntentExtractionResult(canonical, parsed.Parameters);
            return true;
        }

        // Returns catalog spelling (lowercase) or null if the LLM emitted an unknown intent.
        private static string? NormalizeIntentName(string intent)
        {
            var trimmed = intent.Trim();
            foreach (var allowed in AllowedIntents)
            {
                if (string.Equals(trimmed, allowed, StringComparison.OrdinalIgnoreCase))
                {
                    return allowed;
                }
            }

            return null;
        }

        // Verbs that need an explicit object of attention in params.target.
        private static bool RequiresTarget(string canonicalIntent) => canonicalIntent switch
        {
            "inspect" or "move_self" or "attack" or "take" or "talk" => true,
            _ => false
        };

        // Catch-all intent: natural language preserved in params.text.
        private static bool RequiresText(string canonicalIntent) => canonicalIntent == "freeform_action";

        // Strips common ```json fences, then takes the substring from first '{' to last '}'.
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

        // Flattens JSON scalar values to strings for the IntentResponse contract.
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

        private async Task Respond(HttpListenerContext context, int statusCode, object payload)
        {
            var response = context.Response;
            response.StatusCode = statusCode;
            response.ContentType = "application/json";
            // Using object avoids defining a separate DTO for every response shape.
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload, _jsonOptions));
            response.ContentLength64 = bytes.LongLength;
            await response.OutputStream.WriteAsync(bytes);
            response.OutputStream.Close();
        }
        // Limits error payload size when echoing downstream bodies back to the client.
        private static string? TruncateDetails(string? text, int maxLen = 2000)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            return text.Length <= maxLen ? text : text[..maxLen] + "…";
        }
        #endregion
    }
}
