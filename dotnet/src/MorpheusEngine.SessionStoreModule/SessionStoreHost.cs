using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// HTTP host for the <c>session_store</c> module: per-run SQLite open, schema bootstrap, run lifecycle, and turn validate/persist.
/// Essentially a wrapper for RunPersistence which does the actual work. This is just a wrapper that handles the HTTP messaging.
/// </summary>
public sealed class SessionStoreHost
{
    #region Private data

    private readonly HttpListener _listener = new();
    private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
    private readonly RunPersistence _persistence;
    private bool _shutdownRequested = false;

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
        Console.WriteLine($"SessionStore listening on http://127.0.0.1:{port}/");
        Console.WriteLine("SessionStore initialized.");
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
                await RespondJsonAsync(context, 200, new ModuleHealthResponse(true, "session_store", "healthy"));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await RespondJsonAsync(context, 200, new ModuleShutdownResponse(true, "session_store", "Shutdown requested."));
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

            // Run bootstrap: same path name as the router (router forwards JSON to this module).
            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleRequest_initialize(context);
                return;
            }

            if (path.Equals("/validate_turn", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                await HandleRequest_validateTurn(context);
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

    private async Task HandleRequest_initialize(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        RunStartRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<RunStartRequest>(body, JsonOptions);
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

        try
        {
            var response = _persistence.InitializeRun(request.GameProjectId.Trim(), request.RunId.Trim()); // Actual initialization logic.
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

    private async Task HandleRequest_validateTurn(HttpListenerContext context)
    {
        var body = await ReadRequestBodyAsync(context);
        TurnValidateRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<TurnValidateRequest>(body, JsonOptions);
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
            await RespondJsonAsync(context, 400, new ErrorResponse(false, "Request must include non-empty gameProjectId, runId, and turn."));
            return;
        }

        try
        {
            var response = _persistence.ValidateTurn(request.GameProjectId.Trim(), request.RunId.Trim(), request.Turn); // Actual logic.
            await RespondJsonAsync(context, 200, response);
        }
        catch (ArgumentException e)
        {
            await RespondJsonAsync(context, 400, new ErrorResponse(false, e.Message));
        }
        catch (Exception e)
        {
            await RespondJsonAsync(context, 500, new ErrorResponse(false, "Failed to validate turn.", e.Message));
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
            || string.IsNullOrWhiteSpace(request.GameProjectId)
            || string.IsNullOrWhiteSpace(request.RunId)
            || string.IsNullOrWhiteSpace(request.PlayerId)
            || string.IsNullOrWhiteSpace(request.PlayerInput)
            || string.IsNullOrWhiteSpace(request.IntentResponseBody))
        {
            await RespondJsonAsync(
                context,
                400,
                new ErrorResponse(false, "Request must include gameProjectId, runId, turn, playerId, playerInput, and intentResponseBody."));
            return;
        }

        try
        {
            var response = _persistence.PersistTurn(request); // Actual logic.
            await RespondJsonAsync(context, 200, response);
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
