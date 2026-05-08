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

    private static readonly TimeSpan OllamaHttpTimeout = TimeSpan.FromSeconds(60);

    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
    private readonly HttpClient _httpClient = new()
    {
        Timeout = OllamaHttpTimeout
    };
    private readonly HttpListener _listener = new();
    private volatile bool _shutdownRequested = false;
    private volatile bool _runBound = false;
    private volatile bool _initializing = false;
    private int _ollamaPort = 0;
    private string _defaultEmbeddingModel = string.Empty;
    private string _keepAlive = string.Empty;
    private int _numCtx;
    #endregion

    #region Public methods
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
        Console.WriteLine($"ready listen=http://127.0.0.1:{port}/ model='{_defaultEmbeddingModel}' ollama=http://127.0.0.1:{_ollamaPort}/ num_ctx={_numCtx}");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
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
    #endregion

    #region Helpers
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
