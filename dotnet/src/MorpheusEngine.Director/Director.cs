using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the <c>director</c> module: in-memory chat per <c>runId</c>, system prompt from game project files, LLM via router proxy to <c>generic_llm_provider /chat</c>.
/// </summary>
public sealed class Director
{
    #region Nested types

    /// <summary>One in-memory chat message (mirrors Ollama roles used in <see cref="ChatMessageDto"/>).</summary>
    private sealed record ChatMessage(string Role, string Content);

    #endregion

    #region Private data

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();

    private readonly HttpClient _httpClient = new()
    {
        // Narration can exceed intent-extraction latency; fail loud on timeout rather than hang indefinitely.
        Timeout = TimeSpan.FromSeconds(120)
    };

    private readonly HttpListener _listener = new();
    private bool _shutdownRequested = false;

    /// <summary>Per-run conversation: first entry is always <c>system</c> after lazy init.</summary>
    private readonly ConcurrentDictionary<string, List<ChatMessage>> _historyByRunId = new(StringComparer.Ordinal);

    /// <summary>Serializes concurrent <c>/message</c> calls for the same <c>runId</c> so history + LLM round-trip stay consistent.</summary>
    private readonly ConcurrentDictionary<string, SemaphoreSlim> _locksByRunId = new(StringComparer.Ordinal);

    #endregion

    #region Public methods

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
            Console.WriteLine("Director error encountered: " + e.Message);
        }
        finally
        {
            Shutdown();
        }
    }

    public void RequestShutdown() => _shutdownRequested = true;

    #endregion

    #region Private methods

    private void Initialize()
    {
        var ports = EngineConfigLoader.GetPorts();
        var directorPort = ports.GetRequiredPort("director");
        _listener.Prefixes.Add($"http://127.0.0.1:{directorPort}/");
        _listener.Start();
        Console.WriteLine($"Director listening on http://127.0.0.1:{directorPort}/");
        Console.WriteLine("Director initialized.");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
        _httpClient.Dispose();
        Console.WriteLine("Director shut down.");
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
                await Respond(context, 200, new ModuleInfoResponse(true, "director"));
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 200, new ModuleHealthResponse(true, "director", "healthy"));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_shutdown(context);
                return;
            }

            if (path.Equals("/message", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_message(context);
                return;
            }

            await Respond(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            Console.WriteLine("Director encountered unhandled request error: " + e.Message);
            if (context.Response.OutputStream.CanWrite)
            {
                await Respond(context, 500, new ErrorResponse(false, "Unhandled director error.", e.Message));
            }
        }
    }

    private async Task ProcessRequest_shutdown(HttpListenerContext context)
    {
        await Respond(context, 200, new ModuleShutdownResponse(true, "director", "Shutdown requested."));
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

    /// <summary>
    /// Deserializes <see cref="DirectorMessageRequest"/>, ensures per-run history + lock, builds chat messages for Ollama, proxies to LLM, appends user+assistant to history on success, returns <see cref="IntentResponse"/> shim for router/UI.
    /// </summary>
    private async Task ProcessRequest_message(HttpListenerContext context)
    {
        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        DirectorMessageRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<DirectorMessageRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.RunId)
            || string.IsNullOrWhiteSpace(request.GameProjectId)
            || string.IsNullOrWhiteSpace(request.PlayerId)
            || string.IsNullOrWhiteSpace(request.PlayerInput))
        {
            await Respond(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty runId, gameProjectId, playerId, and playerInput."));
            return;
        }

        if (request.Turn < 1)
        {
            await Respond(context, 400, new ErrorResponse(false, "Turn must be >= 1."));
            return;
        }

        var runId = request.RunId.Trim();
        var gameProjectId = request.GameProjectId.Trim();
        var playerInput = request.PlayerInput.Trim();
        var runLock = _locksByRunId.GetOrAdd(runId, static _ => new SemaphoreSlim(1, 1));

        await runLock.WaitAsync();
        try
        {
            // Lazy-init: first message for this run seeds system prompt from disk (fail fast if files missing).
            List<ChatMessage> history;
            try
            {
                history = _historyByRunId.GetOrAdd(
                    runId,
                    _ =>
                    {
                        var systemContent = BuildSystemPromptFromDisk(_configuration.RepositoryRoot, gameProjectId);
                        return new List<ChatMessage> { new ChatMessage("system", systemContent) };
                    });
            }
            catch (FileNotFoundException e)
            {
                await Respond(context, 500, new ErrorResponse(false, e.Message, e.FileName));
                return;
            }
            catch (InvalidOperationException e)
            {
                await Respond(context, 500, new ErrorResponse(false, e.Message));
                return;
            }

            // Build outbound messages without mutating history until the LLM call succeeds (avoids orphan user rows on failure).
            var messagesForApi = new List<ChatMessageDto>(history.Count + 1);
            foreach (var row in history)
            {
                messagesForApi.Add(new ChatMessageDto(row.Role, row.Content));
            }

            messagesForApi.Add(new ChatMessageDto("user", playerInput));

            var model = _configuration.IntentDefaultLlmModel;
            var chatRequest = new ChatGenerateRequest(model, messagesForApi);
            var proxyRequest = new ModuleProxyRequest(
                "director",
                "generic_llm_provider",
                "/chat",
                "POST",
                JsonSerializer.SerializeToElement(chatRequest, JsonOptions));

            using var proxyContent = new StringContent(
                JsonSerializer.Serialize(proxyRequest, JsonOptions),
                Encoding.UTF8,
                "application/json");

            var routerPort = _configuration.GetRequiredListenPort("router");
            HttpResponseMessage llmResponse;
            try
            {
                llmResponse = await _httpClient.PostAsync(
                    $"http://127.0.0.1:{routerPort}/proxy",
                    proxyContent);
            }
            catch (Exception e)
            {
                Console.WriteLine("[Director] Router proxy request failed: " + e.Message);
                await Respond(context, 502, new ErrorResponse(false, "Failed to reach router proxy for LLM chat.", e.Message));
                return;
            }

            using (llmResponse)
            {
                var llmBody = await llmResponse.Content.ReadAsStringAsync();
                if (!llmResponse.IsSuccessStatusCode)
                {
                    Console.WriteLine($"[Director] Router proxy returned {(int)llmResponse.StatusCode}: {llmBody}");
                    await Respond(
                        context,
                        502,
                        new ErrorResponse(
                            false,
                            "Router proxy did not return success for LLM chat.",
                            TruncateDetails(llmBody)));
                    return;
                }

                ChatGenerateResponse? payload;
                try
                {
                    payload = JsonSerializer.Deserialize<ChatGenerateResponse>(llmBody, JsonOptions);
                }
                catch (JsonException e)
                {
                    Console.WriteLine("[Director] Invalid JSON from proxied LLM provider: " + e.Message);
                    await Respond(
                        context,
                        422,
                        new ErrorResponse(false, "Proxied LLM response was not valid JSON.", e.Message));
                    return;
                }

                if (payload is null || !payload.Ok || string.IsNullOrWhiteSpace(payload.Response))
                {
                    Console.WriteLine("[Director] Proxied LLM chat response missing assistant text.");
                    await Respond(
                        context,
                        422,
                        new ErrorResponse(
                            false,
                            "LLM chat response was empty or missing 'response'.",
                            TruncateDetails(llmBody)));
                    return;
                }

                var assistantText = payload.Response.Trim();
                history.Add(new ChatMessage("user", playerInput));
                history.Add(new ChatMessage("assistant", assistantText));

                var narrationParams = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["text"] = assistantText
                };

                await Respond(context, 200, new IntentResponse(true, "narration", narrationParams));
            }
        }
        finally
        {
            runLock.Release();
        }
    }

    /// <summary>
    /// Reads <c>game_projects/&lt;gameProjectId&gt;/system/instructions.md</c> and lore CSV; throws if required files are absent (caller maps to HTTP 500/400).
    /// </summary>
    private static string BuildSystemPromptFromDisk(string repositoryRoot, string gameProjectId)
    {
        var instructionsPath = Path.Combine(repositoryRoot, "game_projects", gameProjectId, "system", "instructions.md");
        var loreCsvPath = Path.Combine(repositoryRoot, "game_projects", gameProjectId, "lore", "default_lore_entries.csv");

        if (!File.Exists(instructionsPath))
        {
            throw new FileNotFoundException($"Director requires instructions at '{instructionsPath}'.");
        }

        if (!File.Exists(loreCsvPath))
        {
            throw new FileNotFoundException($"Director requires lore CSV at '{loreCsvPath}'.");
        }

        var instructions = File.ReadAllText(instructionsPath).Trim();
        var loreSection = BuildCanonLoreSectionFromCsv(loreCsvPath);

        return instructions + Environment.NewLine + Environment.NewLine + loreSection;
    }

    /// <summary>
    /// Parses <c>default_lore_entries.csv</c> (subject + data columns) into a markdown bullet list under <c>## Canon Lore</c>.
    /// </summary>
    private static string BuildCanonLoreSectionFromCsv(string csvPath)
    {
        var lines = File.ReadAllLines(csvPath)
            .Select(static line => line.Trim())
            .Where(static line => line.Length > 0 && !line.StartsWith("#", StringComparison.Ordinal))
            .ToArray();

        if (lines.Length == 0)
        {
            throw new InvalidOperationException($"Lore CSV at '{csvPath}' is empty.");
        }

        var headers = ParseCsvLine(lines[0]).Select(static h => h.ToLowerInvariant()).ToArray();
        var subjectIndex = Array.IndexOf(headers, "subject");
        var dataIndex = Array.FindIndex(
            headers,
            static h => h is "data" or "description" or "entry");
        if (subjectIndex < 0 || dataIndex < 0)
        {
            throw new InvalidOperationException(
                $"Lore CSV at '{csvPath}' must declare 'subject' and 'data' (or description/entry) columns.");
        }

        var sb = new StringBuilder();
        sb.AppendLine("## Canon Lore");
        sb.AppendLine();

        for (var i = 1; i < lines.Length; i++)
        {
            var columns = ParseCsvLine(lines[i]);
            if (subjectIndex >= columns.Count || dataIndex >= columns.Count)
            {
                continue;
            }

            var subject = columns[subjectIndex].Trim();
            var data = columns[dataIndex].Trim();
            if (subject.Length == 0 || data.Length == 0)
            {
                continue;
            }

            sb.Append("- **");
            sb.Append(subject);
            sb.Append(":** ");
            sb.Append(data);
            sb.AppendLine();
        }

        return sb.ToString().TrimEnd();
    }

    /// <summary>Minimal CSV line parser mirroring <c>RunPersistence.ParseCsvLine</c> (quoted fields, doubled quotes).</summary>
    private static List<string> ParseCsvLine(string line)
    {
        var values = new List<string>();
        var current = new StringBuilder();
        var inQuotes = false;
        for (var i = 0; i < line.Length; i++)
        {
            var ch = line[i];
            if (ch == '"')
            {
                if (inQuotes && i + 1 < line.Length && line[i + 1] == '"')
                {
                    current.Append('"');
                    i++;
                }
                else
                {
                    inQuotes = !inQuotes;
                }

                continue;
            }

            if (ch == ',' && !inQuotes)
            {
                values.Add(current.ToString().Trim());
                current.Clear();
                continue;
            }

            current.Append(ch);
        }

        values.Add(current.ToString().Trim());
        for (var v = 0; v < values.Count; v++)
        {
            var s = values[v];
            if (s.Length >= 2 && s[0] == '"' && s[^1] == '"')
            {
                values[v] = s.Substring(1, s.Length - 2).Trim();
            }
        }

        return values;
    }

    private static string? TruncateDetails(string? text, int maxLen = 2000)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        return text.Length <= maxLen ? text : text[..maxLen] + "…";
    }

    private async Task Respond(HttpListenerContext context, int statusCode, object payload)
    {
        var response = context.Response;
        response.StatusCode = statusCode;
        response.ContentType = "application/json";
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload, JsonOptions));
        response.ContentLength64 = bytes.LongLength;
        await response.OutputStream.WriteAsync(bytes);
        response.OutputStream.Close();
    }

    #endregion
}
