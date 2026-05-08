using System.Diagnostics;
using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine.TestModuleHost;

public static class TestModuleHostMarker
{
}

internal sealed record TestModuleBehavior(
    int Port,
    string ModuleName,
    string EventLogPath,
    string? PidFilePath = null,
    bool ExitBeforeListening = false,
    int ExitCode = 1,
    string InitialHealthStatus = "awaiting_initialize",
    bool InitialHealthOk = false,
    int InitializeResponseStatusCode = 200,
    string HealthStatusAfterInitialize = "healthy",
    bool HealthOkAfterInitialize = true,
    bool InitializedAfterInitialize = true,
    bool IgnoreShutdown = false,
    bool SpawnOrphanChildOnStart = false,
    string? ChildPidFilePath = null);

internal sealed class Program
{
    private static readonly JsonSerializerOptions JSON_OPTIONS = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private static readonly object FILE_SYNC = new();
    private static volatile bool _shutdownRequested = false;
    private static string _healthStatus = "awaiting_initialize";
    private static bool _healthOk = false;
    private static bool _initialized = false;
    private static TestModuleBehavior _behavior = null!;
    private static Process? _orphanChild;

    public static async Task<int> Main()
    {
        _behavior = LoadBehavior();
        _healthStatus = _behavior.InitialHealthStatus;
        _healthOk = _behavior.InitialHealthOk;
        _initialized = false;
        WriteCurrentPid();

        if (_behavior.SpawnOrphanChildOnStart)
        {
            StartOrphanChild();
        }

        if (_behavior.ExitBeforeListening)
        {
            AppendEvent("exit_before_listen");
            return _behavior.ExitCode;
        }

        using var listener = new HttpListener();
        listener.Prefixes.Add($"http://127.0.0.1:{_behavior.Port}/");
        listener.Start();
        AppendEvent("started");

        while (!_shutdownRequested)
        {
            HttpListenerContext context;
            try
            {
                context = await listener.GetContextAsync();
            }
            catch (HttpListenerException) when (_shutdownRequested)
            {
                break;
            }
            catch (ObjectDisposedException) when (_shutdownRequested)
            {
                break;
            }

            _ = ProcessRequestAsync(context, listener);
        }

        return 0;
    }

    private static TestModuleBehavior LoadBehavior()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "behavior.json");
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("Expected behavior.json beside the test module host assembly.", path);
        }

        return JsonSerializer.Deserialize<TestModuleBehavior>(File.ReadAllText(path), JSON_OPTIONS)
            ?? throw new InvalidOperationException("behavior.json did not deserialize.");
    }

    private static void WriteCurrentPid()
    {
        if (string.IsNullOrWhiteSpace(_behavior.PidFilePath))
        {
            return;
        }

        File.WriteAllText(_behavior.PidFilePath, Environment.ProcessId.ToString());
    }

    private static void StartOrphanChild()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        var psi = new ProcessStartInfo
        {
            FileName = "powershell",
            Arguments = "-NoProfile -Command \"Start-Sleep -Seconds 300\"",
            UseShellExecute = false,
            CreateNoWindow = true
        };
        _orphanChild = Process.Start(psi)
            ?? throw new InvalidOperationException("Failed to start orphan child process.");
        if (!string.IsNullOrWhiteSpace(_behavior.ChildPidFilePath))
        {
            File.WriteAllText(_behavior.ChildPidFilePath, _orphanChild.Id.ToString());
        }

        AppendEvent("spawned_child");
    }

    private static async Task ProcessRequestAsync(HttpListenerContext context, HttpListener listener)
    {
        try
        {
            var path = context.Request.Url?.AbsolutePath ?? "/";
            var method = context.Request.HttpMethod.Trim().ToUpperInvariant();

            if (path.Equals("/health", StringComparison.OrdinalIgnoreCase) && method == "GET")
            {
                await RespondAsync(context, 200, new ModuleHealthResponse(_healthOk, _healthStatus, _initialized));
                return;
            }

            if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                AppendEvent("initialize");
                _healthStatus = _behavior.HealthStatusAfterInitialize;
                _healthOk = _behavior.HealthOkAfterInitialize;
                _initialized = _behavior.InitializedAfterInitialize;
                await RespondAsync(context, _behavior.InitializeResponseStatusCode, new InitializeModuleResponse(true));
                return;
            }

            if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase) && method == "POST")
            {
                AppendEvent("shutdown");
                await RespondAsync(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
                if (!_behavior.IgnoreShutdown)
                {
                    _shutdownRequested = true;
                    listener.Stop();
                }

                return;
            }

            await RespondAsync(context, 404, new ErrorResponse(false, "Not found: " + path));
        }
        catch (Exception e)
        {
            if (context.Response.OutputStream.CanWrite)
            {
                await RespondAsync(context, 500, new ErrorResponse(false, "Test module host request failed.", e.Message));
            }
        }
    }

    private static async Task RespondAsync(HttpListenerContext context, int statusCode, object payload)
    {
        var response = context.Response;
        response.StatusCode = statusCode;
        response.ContentType = "application/json";
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload));
        response.ContentLength64 = bytes.LongLength;
        await response.OutputStream.WriteAsync(bytes);
        response.OutputStream.Close();
    }

    private static void AppendEvent(string eventName)
    {
        if (string.IsNullOrWhiteSpace(_behavior.EventLogPath))
        {
            return;
        }

        lock (FILE_SYNC)
        {
            File.AppendAllText(_behavior.EventLogPath, $"{eventName}:{_behavior.ModuleName}{Environment.NewLine}");
        }
    }
}
