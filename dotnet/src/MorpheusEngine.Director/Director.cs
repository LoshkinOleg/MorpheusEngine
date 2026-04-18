using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the director module: one in-process run at a time, system prompt from game project files, LLM via router proxy to generic_llm_provider /chat.
/// </summary>
public sealed class Director
{
    #region Nested types

    /// <summary>One in-memory chat message (mirrors Ollama roles used in <see cref="ChatGenerateRequest.ChatMessageDto"/>).</summary>
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
        Timeout = TimeSpan.FromSeconds(30)
    };

    private readonly HttpListener _listener = new();
    private bool _shutdownRequested = false;

    /// <summary>Single-flight gate for /initialize and /message (one active run per Director process).</summary>
    private readonly SemaphoreSlim _sessionGate = new(1, 1);

    /// <summary>Set after successful POST /initialize; cleared only when the process restarts.</summary>
    private bool _initialized = false;

    /// <summary>Conversation for the bound run; first entry is always system after /initialize.</summary>
    private List<ChatMessage>? _history = null;

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
        _sessionGate.Dispose();
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
                await Respond(context, 200, new ModuleHealthResponse(true, "healthy"));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_shutdown(context);
                return;
            }

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase))
            {
                await ProcessRequest_initialize(context);
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
        await Respond(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
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
    /// Accepts <see cref="InitializeModuleRequest"/>, loads system prompt from disk once, seeds in-memory history; returns <see cref="InitializeModuleResponse"/> on success. Second call without process restart yields 409 (fail-fast).
    /// </summary>
    private async Task ProcessRequest_initialize(HttpListenerContext context)
    {
        if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
        {
            await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
            return;
        }

        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        InitializeModuleRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.RunId)
            || string.IsNullOrWhiteSpace(request.GameProjectId))
        {
            await Respond(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty runId and gameProjectId."));
            return;
        }

        var runId = request.RunId.Trim();
        var gameProjectId = request.GameProjectId.Trim();

        await _sessionGate.WaitAsync();
        try
        {
            if (_initialized)
            {
                await Respond(
                    context,
                    409,
                    new ErrorResponse(
                        false,
                        "Director already initialized for this process; restart the Director module to start another run."));
                return;
            }

            string systemContent;
            try
            {
                systemContent = BuildSystemPromptFromDisk(_configuration.RepositoryRoot, gameProjectId);
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

            _history = new List<ChatMessage> { new ChatMessage("system", systemContent) };
            _initialized = true;

            await Respond(context, 200, new InitializeModuleResponse(true));
        }
        finally
        {
            _sessionGate.Release();
        }
    }

    /// <summary>
    /// Deserializes <see cref="DirectorMessageRequest"/>; requires prior POST /initialize; builds chat messages for Ollama, proxies to LLM, appends user+assistant to history on success, returns <see cref="IntentResponse"/> shim for router/UI.
    /// </summary>
    private async Task ProcessRequest_message(HttpListenerContext context)
    {
        if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
        {
            await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
            return;
        }

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

        if (request is null || string.IsNullOrWhiteSpace(request.PlayerInput))
        {
            await Respond(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty playerInput."));
            return;
        }

        if (request.Turn < 1)
        {
            await Respond(context, 400, new ErrorResponse(false, "Turn must be >= 1."));
            return;
        }

        var playerInput = request.PlayerInput.Trim();

        await _sessionGate.WaitAsync();
        try
        {
            if (!_initialized || _history is null)
            {
                await Respond(
                    context,
                    400,
                    new ErrorResponse(false, "Director is not initialized; call POST /initialize first."));
                return;
            }

            var history = _history;

            // Build outbound messages without mutating history until the LLM call succeeds (avoids orphan user rows on failure).
            var messagesForApi = new List<ChatGenerateRequest.ChatMessageDto>(history.Count + 1);
            foreach (var row in history)
            {
                messagesForApi.Add(new ChatGenerateRequest.ChatMessageDto(row.Role, row.Content));
            }

            messagesForApi.Add(new ChatGenerateRequest.ChatMessageDto("user", playerInput)); // Add new player input.

            var chatRequest = new ChatGenerateRequest { Messages = messagesForApi };
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

            // I'm noticing that the routing mechanism makes reading endpoint handling code quite cumbersome. I wonder if there's some elegant way to avoid that.
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
            _sessionGate.Release();
        }
    }

    /// <summary>
    /// Reads game_projects/&lt;gameProjectId&gt;/system/instructions.md and lore CSV; throws if required files are absent (caller maps to HTTP 500/400).
    /// </summary>
    private static string BuildSystemPromptFromDisk(string repositoryRoot, string gameProjectId)
    {
        // TODO: I feel like this should be part of EngineConfigLoader instead.

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
    /// Parses default_lore_entries.csv (subject + data columns) into a markdown bullet list under ## Canon Lore.
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

    /// <summary>Minimal CSV line parser mirroring RunPersistence.ParseCsvLine (quoted fields, doubled quotes).</summary>
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
