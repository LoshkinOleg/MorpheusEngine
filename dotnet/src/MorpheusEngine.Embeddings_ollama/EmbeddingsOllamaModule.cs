using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

public sealed class EmbeddingsOllamaModule
{
    #region Nested types
    private sealed record OllamaEmbedOptions(int num_ctx);

    private sealed record OllamaEmbedRequest(
        string model,
        IReadOnlyList<string> input,
        bool truncate,
        string keep_alive,
        OllamaEmbedOptions options);
    #endregion

    #region Private data
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private static readonly TimeSpan OllamaReadyTimeout = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan OllamaReadyPollInterval = TimeSpan.FromMilliseconds(200);
    private static readonly TimeSpan OllamaHttpTimeout = TimeSpan.FromSeconds(60);

    private readonly EngineConfiguration _configuration;
    private readonly HttpClient _httpClient;
    private readonly HttpListener _listener = new();
    private readonly CancellationTokenSource _shutdownCts = new();
    private volatile bool _shutdownRequested = false;
    private volatile bool _runBound = false;
    private volatile bool _initializing = false;
    private volatile bool _ollamaReady = false;
    private volatile bool _ollamaBootstrapFailed = false;
    private Task? _ollamaReadyProbeTask;
    private int _ollamaPort = 0;
    private string _defaultEmbeddingModel = string.Empty;
    private string _keepAlive = string.Empty;
    private int _numCtx;
    #endregion

    #region Public methods
    public EmbeddingsOllamaModule()
        : this(
            EngineConfigLoader.GetConfiguration(),
            new HttpClient
            {
                Timeout = OllamaHttpTimeout
            })
    {
    }

    internal EmbeddingsOllamaModule(EngineConfiguration configuration, HttpClient httpClient)
    {
        _configuration = configuration ?? throw new ArgumentNullException(nameof(configuration));
        _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
    }

    public async Task RunAsync()
    {
        Initialize();

        try
        {
            while (!_shutdownRequested)
            {
                HttpListenerContext context;
                try
                {
                    context = await _listener.GetContextAsync();
                }
                catch (HttpListenerException) when (_shutdownRequested)
                {
                    break;
                }
                catch (ObjectDisposedException) when (_shutdownRequested)
                {
                    break;
                }

                _ = ProcessQueryAsync(context);
            }
        }
        catch (HttpListenerException e)
        {
            Console.WriteLine("Embeddings_ollama error: " + e.Message);
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
        var module = _configuration.GetRequiredGenericEmbeddingsModule();
        var options = module.EmbeddingsOptions
            ?? throw new InvalidOperationException("embeddings_ollama: generic_embeddings target module has no embeddings options.");
        _ollamaPort = options.OllamaPort;
        _defaultEmbeddingModel = options.DefaultEmbeddingModel.Trim();
        _keepAlive = options.KeepAlive.Trim();
        _numCtx = options.NumCtx;

        var port = _configuration.GetRequiredListenPort("embeddings_ollama");
        _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        _listener.Start();
        _ollamaReadyProbeTask = ProbeOllamaReadyAsync(_shutdownCts.Token);
        Console.WriteLine($"ready listen=http://127.0.0.1:{port}/ model='{_defaultEmbeddingModel}' ollama=http://127.0.0.1:{_ollamaPort}/ num_ctx={_numCtx} awaiting_ollama=true");
    }

    private void Shutdown()
    {
        _shutdownRequested = true;
        _shutdownCts.Cancel();

        if (_ollamaReadyProbeTask is { IsCompleted: false })
        {
            try
            {
                _ollamaReadyProbeTask.Wait(TimeSpan.FromSeconds(5));
            }
            catch
            {
                // Best-effort wait so a cancelled readiness probe does not block teardown.
            }
        }

        _listener.Stop();
        _listener.Close();
        _shutdownCts.Dispose();
        _httpClient.Dispose();
        Console.WriteLine("Embeddings_ollama shut down.");
    }

    private async Task ProcessQueryAsync(HttpListenerContext context)
    {
        try
        {
            if (context.Request.Url is null)
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid request URL."));
                return;
            }

            var path = context.Request.Url.AbsolutePath;
            var method = context.Request.HttpMethod.Trim().ToUpperInvariant();
            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                await RespondJsonAsync(context, 200, new { ok = true, moduleName = "embeddings_ollama", provider = "ollama", model = _defaultEmbeddingModel });
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                if (_initializing)
                {
                    await RespondJsonAsync(context, 503, new ModuleHealthResponse(false, "initializing", false));
                    return;
                }

                if (_ollamaBootstrapFailed)
                {
                    await RespondJsonAsync(context, 200, new ModuleHealthResponse(false, "ollama_startup_failed", false));
                    return;
                }

                if (!_ollamaReady)
                {
                    await RespondJsonAsync(context, 200, new ModuleHealthResponse(false, "ollama_starting", false));
                    return;
                }

                await RespondJsonAsync(
                    context,
                    200,
                    _runBound
                        ? new ModuleHealthResponse(true, "healthy", true)
                        : new ModuleHealthResponse(false, "awaiting_initialize", false));
                return;
            }

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await ProcessInitializeAsync(context);
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await RespondJsonAsync(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
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

            if ((path.Equals("/embed", StringComparison.OrdinalIgnoreCase) || path.Equals("/embed_batch", StringComparison.OrdinalIgnoreCase))
                && method == "POST")
            {
                await ProcessEmbedAsync(context);
                return;
            }

            if (path.Equals("/token_count", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await ProcessTokenCountAsync(context);
                return;
            }

            await RespondJsonAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "embeddings_ollama request failed.", e.Message));
        }
    }

    private async Task ProcessInitializeAsync(HttpListenerContext context)
    {
        _initializing = true;
        try
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            var request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, JsonOptions);
            if (request is null
                || string.IsNullOrWhiteSpace(request.GameProjectId)
                || string.IsNullOrWhiteSpace(request.RunId))
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, "Initialize requires non-empty gameProjectId and runId."));
                return;
            }

            _runBound = true;
            await RespondJsonAsync(context, 200, new InitializeModuleResponse(true));
        }
        finally
        {
            _initializing = false;
        }
    }

    private async Task ProcessEmbedAsync(HttpListenerContext context)
    {
        if (!_runBound)
        {
            await RespondJsonAsync(context, 409, new ErrorResponse(false, "Module must be initialized before /embed."));
            return;
        }

        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        EmbeddingRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<EmbeddingRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null || request.Texts.Count == 0)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include a non-empty texts array."));
            return;
        }

        if (request.Texts.Any(static text => string.IsNullOrWhiteSpace(text)))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Embedding texts must be non-empty."));
            return;
        }

        if (!await RespondIfOllamaUnavailableAsync(context))
        {
            return;
        }

        var model = string.IsNullOrWhiteSpace(request.Model) ? _defaultEmbeddingModel : request.Model.Trim();
        var ollamaRequest = new OllamaEmbedRequest(
            model,
            request.Texts.Select(static text => text.Trim()).ToArray(),
            truncate: false,
            _keepAlive,
            new OllamaEmbedOptions(_numCtx));
        var requestJson = JsonSerializer.Serialize(ollamaRequest, JsonOptions);
        using var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

        HttpResponseMessage ollamaResponse;
        try
        {
            ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/embed"), content);
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 503, new ErrorResponse(false, "Ollama embedding endpoint is unavailable.", e.Message));
            return;
        }

        var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
        if (!ollamaResponse.IsSuccessStatusCode)
        {
            await RespondJsonAsync(context, (int)ollamaResponse.StatusCode, new ErrorResponse(false, "Ollama returned an embedding error.", ollamaBody));
            return;
        }

        using var doc = JsonDocument.Parse(ollamaBody);
        if (!doc.RootElement.TryGetProperty("embeddings", out var embeddingsElement)
            || embeddingsElement.ValueKind != JsonValueKind.Array)
        {
            await RespondJsonAsync(context, 502, new ErrorResponse(false, "Ollama embedding response did not contain an embeddings array.", ollamaBody));
            return;
        }

        var vectors = new List<EmbeddingVectorDto>();
        var dimensions = -1;
        var index = 0;
        foreach (var embeddingElement in embeddingsElement.EnumerateArray())
        {
            if (embeddingElement.ValueKind != JsonValueKind.Array)
            {
                await RespondJsonAsync(context, 502, new ErrorResponse(false, "Ollama embedding response contained a non-array vector.", ollamaBody));
                return;
            }

            var vector = embeddingElement.EnumerateArray()
                .Select(static value => value.GetSingle())
                .ToArray();
            if (vector.Length == 0)
            {
                await RespondJsonAsync(context, 502, new ErrorResponse(false, "Ollama embedding response contained an empty vector.", ollamaBody));
                return;
            }

            dimensions = dimensions < 0 ? vector.Length : dimensions;
            if (vector.Length != dimensions)
            {
                await RespondJsonAsync(context, 502, new ErrorResponse(false, "Ollama embedding response contained inconsistent vector dimensions.", ollamaBody));
                return;
            }

            vectors.Add(new EmbeddingVectorDto(index, vector));
            index++;
        }

        if (vectors.Count != request.Texts.Count)
        {
            await RespondJsonAsync(context, 502, new ErrorResponse(false, "Ollama embedding count did not match input text count.", ollamaBody));
            return;
        }

        await RespondJsonAsync(context, 200, new EmbeddingResponse(true, model, dimensions, vectors));
    }

    private async Task ProcessTokenCountAsync(HttpListenerContext context)
    {
        if (!_runBound)
        {
            await RespondJsonAsync(context, 409, new ErrorResponse(false, "Module must be initialized before /token_count."));
            return;
        }

        string body;
        using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
        {
            body = await reader.ReadToEndAsync();
        }

        TokenCountRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<TokenCountRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null || string.IsNullOrWhiteSpace(request.Text))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty text."));
            return;
        }

        if (!string.IsNullOrWhiteSpace(request.Model)
            && !string.Equals(request.Model.Trim(), _defaultEmbeddingModel, StringComparison.OrdinalIgnoreCase))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, $"embeddings_ollama token_count uses configured model '{_defaultEmbeddingModel}', not caller model '{request.Model.Trim()}'."));
            return;
        }

        if (!await RespondIfOllamaUnavailableAsync(context))
        {
            return;
        }

        var ollamaPayload = new
        {
            model = _defaultEmbeddingModel,
            prompt = request.Text,
            stream = false,
            raw = true,
            truncate = false,
            options = new { num_ctx = _numCtx, num_predict = 0 }
        };
        var requestJson = JsonSerializer.Serialize(ollamaPayload);
        using var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

        HttpResponseMessage ollamaResponse;
        try
        {
            // Ollama reports prompt_eval_count on /api/generate responses; use num_predict=0 so we count without generating text.
            ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/generate"), content);
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 503, new ErrorResponse(false, "Ollama token_count endpoint is unavailable.", e.Message));
            return;
        }

        var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
        if (!ollamaResponse.IsSuccessStatusCode)
        {
            await RespondJsonAsync(context, (int)ollamaResponse.StatusCode, new ErrorResponse(false, "Ollama returned an error during token_count.", ollamaBody));
            return;
        }

        if (TryReadPromptEvalCount(ollamaBody, out var exactTokens))
        {
            await RespondJsonAsync(context, 200, new TokenCountResponse(true, _defaultEmbeddingModel, exactTokens, true));
            return;
        }

        await RespondJsonAsync(context, 200, new TokenCountResponse(true, _defaultEmbeddingModel, EstimateTokensFromText(request.Text), false));
    }
    #endregion

    #region Helpers
    private async Task ProbeOllamaReadyAsync(CancellationToken cancellationToken)
    {
        Exception? lastError = null;
        var deadline = DateTime.UtcNow + OllamaReadyTimeout;

        while (DateTime.UtcNow < deadline && !cancellationToken.IsCancellationRequested)
        {
            try
            {
                using var response = await _httpClient.GetAsync(BuildOllamaUri("/"), cancellationToken);
                if (response.IsSuccessStatusCode)
                {
                    _ollamaReady = true;
                    return;
                }

                lastError = new InvalidOperationException($"Health probe returned {(int)response.StatusCode}.");
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception e)
            {
                lastError = e;
            }

            try
            {
                await Task.Delay(OllamaReadyPollInterval, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
        }

        if (!_shutdownRequested)
        {
            _ollamaBootstrapFailed = true;
            Console.WriteLine("Embeddings_ollama: configured Ollama did not become ready. " + (lastError?.Message ?? "No additional error details."));
        }
    }

    private async Task<bool> RespondIfOllamaUnavailableAsync(HttpListenerContext context)
    {
        if (_ollamaReady)
        {
            return true;
        }

        var detail = _ollamaBootstrapFailed
            ? $"Configured Ollama failed to become ready on port {_ollamaPort}."
            : $"Configured Ollama is still starting on port {_ollamaPort}.";
        await RespondJsonAsync(context, 503, new ErrorResponse(false, "Configured Ollama is not ready.", detail));
        return false;
    }

    private static bool TryReadPromptEvalCount(string ollamaBody, out int promptEvalCount)
    {
        promptEvalCount = 0;
        try
        {
            using var doc = JsonDocument.Parse(ollamaBody);
            if (!doc.RootElement.TryGetProperty("prompt_eval_count", out var countElement)
                || countElement.ValueKind != JsonValueKind.Number
                || !countElement.TryGetInt32(out var count)
                || count <= 0)
            {
                return false;
            }

            promptEvalCount = count;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static int EstimateTokensFromText(string text)
    {
        var utf8Bytes = Encoding.UTF8.GetByteCount(text);
        return Math.Max(1, (int)Math.Ceiling(utf8Bytes / 4.0));
    }

    private string BuildOllamaUri(string path) =>
        $"http://127.0.0.1:{_ollamaPort}{EngineConfiguration.NormalizePath(path)}";

    private static async Task RespondJsonAsync(HttpListenerContext context, int statusCode, object payload)
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
