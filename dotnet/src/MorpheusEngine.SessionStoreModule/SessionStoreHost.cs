using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the session_store module: per-run SQLite open, schema bootstrap, run lifecycle, and turn persist.
/// POST /persist_turn targets the run established by the last successful host POST /initialize on this process (one run per process).
/// Essentially a wrapper for RunPersistence which does the actual work. This is just a wrapper that handles the HTTP messaging.
/// </summary>
public sealed class SessionStoreHost : IEngineRunBinder
{
    #region Private data

    private readonly HttpListener _listener = new();
    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
    private readonly RunPersistence _persistence;
    private readonly object _sessionLock = new();
    private volatile bool _initializing;
    private bool _shutdownRequested = false;
    /// <summary>Trimmed game project id from the last successful POST /initialize; empty until then.</summary>
    private string _boundGameProjectId = string.Empty;
    /// <summary>Trimmed run id from the last successful POST /initialize; empty until then.</summary>
    private string _boundRunId = string.Empty;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    #endregion

    public SessionStoreHost()
    {
        _persistence = new RunPersistence(_configuration.RepositoryRoot);
    }

    #region Public methods

    public bool IsRunBound => !string.IsNullOrWhiteSpace(_boundRunId) && !string.IsNullOrWhiteSpace(_boundGameProjectId);

    public async Task BindRunAsync(InitializeModuleRequest request, CancellationToken cancellationToken)
    {
        if (request is null
            || string.IsNullOrWhiteSpace(request.GameProjectId)
            || string.IsNullOrWhiteSpace(request.RunId))
        {
            throw new ArgumentException("Request must include non-empty gameProjectId and runId.", nameof(request));
        }

        cancellationToken.ThrowIfCancellationRequested();
        var response = _persistence.InitializeRun(request.GameProjectId.Trim(), request.RunId.Trim());
        _boundGameProjectId = request.GameProjectId.Trim();
        _boundRunId = request.RunId.Trim();
        Console.WriteLine($"[SessionStore] Bound run runId={_boundRunId} gameProjectId={_boundGameProjectId}.");
        _ = response; // Response is returned by HTTP handler; retained here to keep the same code path.
        await Task.CompletedTask;
    }

    public async Task RunAsync()
    {
        Initialize();

        try
        {
            while (!_shutdownRequested)
            {
                var context = await _listener.GetContextAsync();
                _ = ProcessQueryAsync(context);
            }
        }
        catch (HttpListenerException e)
        {
            Console.WriteLine("SessionStoreHost error: " + e.Message);
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
        var port = _configuration.GetRequiredListenPort("session_store");
        _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        _listener.Start();
        Console.WriteLine($"ready listen=http://127.0.0.1:{port}/");
    }

    private void Shutdown()
    {
        _listener.Stop();
        _listener.Close();
        Console.WriteLine("SessionStore shut down.");
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
                await RespondJsonAsync(context, 200, new ModuleInfoResponse(true, "session_store"));
                return;
            }

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                if (_initializing)
                {
                    await RespondJsonAsync(
                        context,
                        503,
                        new ModuleHealthResponse(false, "initializing", false));
                    return;
                }

                bool runBound;
                lock (_sessionLock)
                {
                    runBound = IsRunBound;
                }

                if (runBound)
                {
                    await RespondJsonAsync(context, 200, new ModuleHealthResponse(true, "healthy", true));
                    return;
                }

                await RespondJsonAsync(context, 200, new ModuleHealthResponse(false, "awaiting_initialize", false));
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

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleRequest_bindRun(context);
                return;
            }

            if (path.Equals("/persist_turn", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleRequest_persistTurn(context);
                return;
            }

            await RespondJsonAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            Console.WriteLine("SessionStore unhandled request error: " + e.Message);
            if (context.Response.OutputStream.CanWrite)
            {
                await RespondJsonAsync(context, 500, new ErrorResponse(false, "Unhandled session_store error.", e.Message));
            }
        }
    }

    private async Task HandleRequest_bindRun(HttpListenerContext context)
    {
        if (!IsLoopbackRequest(context))
        {
            await RespondJsonAsync(context, 403, new ErrorResponse(false, "POST /initialize is only allowed from loopback."));
            return;
        }

        var body = await ReadRequestBodyAsync(context);
        InitializeModuleRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.GameProjectId)
            || string.IsNullOrWhiteSpace(request.RunId))
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty gameProjectId and runId."));
            return;
        }

        _initializing = true;
        try
        {
            try
            {
                InitializeModuleResponse response;
                lock (_sessionLock)
                {
                    response = _persistence.InitializeRun(request.GameProjectId.Trim(), request.RunId.Trim());
                    _boundGameProjectId = request.GameProjectId.Trim();
                    _boundRunId = request.RunId.Trim();
                }

                await RespondJsonAsync(context, 200, response);
            }
            catch (ArgumentException e)
            {
                await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
            }
            catch (Exception e)
            {
                await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to initialize run store.", e.Message));
            }
        }
        finally
        {
            _initializing = false;
        }
    }

    private async Task HandleRequest_persistTurn(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        TurnPersistRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<TurnPersistRequest>(body, JsonOptions);
        }
        catch (JsonException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
            return;
        }

        if (request is null
            || string.IsNullOrWhiteSpace(request.PlayerInput)
            || string.IsNullOrWhiteSpace(request.DirectorResponseBody))
        {
            await RespondJsonAsync(
                context,
                400,
                new ErrorResponse(false, "Request must include non-empty turn, playerInput, and directorResponseBody."));
            return;
        }

        try
        {
            TurnPersistResponse? response = null;
            bool missingBound;
            lock (_sessionLock)
            {
                missingBound = string.IsNullOrEmpty(_boundGameProjectId) || string.IsNullOrEmpty(_boundRunId);
                if (!missingBound)
                {
                    response = _persistence.PersistTurn(_boundGameProjectId, _boundRunId, request);
                }
            }

            if (missingBound)
            {
                await RespondJsonAsync(
                    context,
                    400,
                    new ErrorResponse(
                        false,
                        "No bound run: the host must bind the run on this session_store process before POST /persist_turn."));
                return;
            }

            await RespondJsonAsync(context, 200, response!);
        }
        catch (ArgumentException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
        }
        catch (InvalidOperationException e)
        {
            await RespondJsonAsync(context, 409, new ErrorResponse(false, e.Message));
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to persist turn.", e.Message));
        }
    }

    #endregion

    private static bool IsLoopbackRequest(HttpListenerContext context)
    {
        var ep = context.Request.RemoteEndPoint;
        return ep is null || System.Net.IPAddress.IsLoopback(ep.Address);
    }

    #region Helpers
    private static async Task<string> ReadRequestBodyAsync(HttpListenerContext context)
    {
        using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
        return await reader.ReadToEndAsync();
    }
    private static async Task RespondJsonAsync(HttpListenerContext context, int statusCode, object payload)
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
