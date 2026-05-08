using System.Diagnostics;
using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    /// <summary>
    /// Host-side process wrapper for a module implementation (for example Director or IntentExtractor).
    /// Spawns the implementation in a separate process and provides the host-facing interface for startup, initialization, health checks, and shutdown.
    /// Module implementations should log via Console methods only; the host prepends unified prefixes when forwarding child output.
    /// </summary>
    public sealed class ManagedModule
    {
        private sealed class TerminalModuleHealthException : InvalidOperationException
        {
            public TerminalModuleHealthException(string message)
                : base(message)
            {
            }
        }

        #region Public data
        public string DisplayName => _definition.DisplayName;
        public string PortKey => _definition.PortKey; // PortKey =/= port! PortKey is the name of the module, like "intent_extractor" that can be used to resolve to the module's actual port.
        public int Port { get; } = 0;
        public bool Required { get; }
        #endregion

        #region Private data
        // Intentional exception to "public static readonly" guidance: these are mutable framework objects and are kept private to
        // prevent external callers from mutating shared process-wide behavior.
        private readonly HttpClient _httpClient;
        private static readonly JsonSerializerOptions HealthJsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };
        private readonly EngineConfiguration _configuration;
        private readonly EngineModuleInfo _definition;
        private Process _process = new Process();
        /// <summary>True after <see cref="StartProcess"/> successfully spawned a child; <see cref="StopAsync"/> is a no-op until then.</summary>
        private bool _childProcessSpawned = false;
        #endregion

        #region Public methods
        public ManagedModule(EngineConfiguration configuration, EngineModuleInfo definition, bool requiredForRun)
            : this(
                configuration,
                definition,
                requiredForRun,
                new HttpClient
                {
                    Timeout = TimeSpan.FromSeconds(3)
                })
        {
        }

        internal ManagedModule(
            EngineConfiguration configuration,
            EngineModuleInfo definition,
            bool requiredForRun,
            HttpClient httpClient)
        {
            _configuration = configuration ?? throw new ArgumentNullException(nameof(configuration));
            _definition = definition ?? throw new ArgumentNullException(nameof(definition));
            _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
            Port = _configuration.GetRequiredListenPort(definition.PortKey);
            Required = requiredForRun;
        }

        /// <summary>
        /// Starts the child process that runs the module implementation.
        /// </summary>
        /// <param name="moduleHostJob">When non-null (Windows host), the spawned module process is assigned so it cannot outlive the job handle.</param>
        public void StartProcess(WindowsJobObject? moduleHostJob)
        {
            // Spawn first so later health checks can fail fast on child exit.
            var psi = CreateProcessStartInfo();
            _process = Process.Start(psi) ?? throw new InvalidOperationException($"Failed to start {DisplayName}.");
            if (moduleHostJob is not null)
            {
                moduleHostJob.AssignProcess(_process);
            }

            _process.OutputDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    ForwardChildLine(e.Data, isError: false);
                }
            };
            _process.ErrorDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    ForwardChildLine(e.Data, isError: true);
                }
            };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
            _childProcessSpawned = true;
        }

        /// <summary>
        /// Waits until GET /health returns 2xx with <see cref="ModuleHealthResponse.Initialized"/> false (pre-POST /initialize accepting state).
        /// Implementations may use status ollama_starting with 200 while a child process is still booting; the host treats that as "listening" and proceeds.
        /// </summary>
        public async Task WaitForStartProcessCompletedAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            var deadline = DateTime.UtcNow + timeout;
            Exception? lastError = null;

            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();

                // If the process dies during bootstrap, waiting longer can never succeed.
                if (_process.HasExited)
                {
                    throw new InvalidOperationException($"{DisplayName} exited before it began listening.");
                }

                try
                {
                    using var response = await _httpClient.GetAsync(GetModuleUri("/health"), cancellationToken);
                    var body = await response.Content.ReadAsStringAsync(cancellationToken);
                    var health = JsonSerializer.Deserialize<ModuleHealthResponse>(body, HealthJsonOptions);

                    // llm_provider_qwen may return 200 ollama_startup_failed / initialize_failed with a JSON body even when HTTP is non-2xx for some paths; treat as terminal.
                    if (string.Equals(health?.Status, "ollama_startup_failed", StringComparison.OrdinalIgnoreCase)
                        || string.Equals(health?.Status, "initialize_failed", StringComparison.OrdinalIgnoreCase))
                    {
                        throw new TerminalModuleHealthException(
                            $"{DisplayName} reported terminal health status '{health?.Status}' while waiting for listen (see module logs). Body: {body}");
                    }

                    if (!response.IsSuccessStatusCode)
                    {
                        lastError = new InvalidOperationException(
                            $"{DisplayName} /health returned {(int)response.StatusCode} (status={health?.Status ?? "(null)"}).");
                    }
                    else if (health is not null && !health.Initialized)
                    {
                        return;
                    }
                    else
                    {
                        lastError = new InvalidOperationException(
                            $"{DisplayName} /health returned 2xx but missing or unexpected body (expected initialized=false).");
                    }
                }
                catch (TerminalModuleHealthException)
                {
                    throw;
                }
                catch (Exception e)
                {
                    lastError = e;
                }

                await Task.Delay(250, cancellationToken);
            }

            throw new TimeoutException($"Timed out waiting for {DisplayName} to begin listening. {lastError?.Message}");
        }

        /// <summary>
        /// Host-driven initialization call.
        /// Sends POST /initialize to the module implementation process so it binds to the run and prepares runtime state.
        /// Implementations may return 202 Accepted with the same JSON shape as 200 to defer long work; the host then relies on
        /// <see cref="WaitForInitializationToCompleteAsync"/> (GET /health) for completion. Any 2xx response is treated as acceptance.
        /// </summary>
        public async Task InitializeAsync(
            InitializeModuleRequest request,
            TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            if (!_childProcessSpawned)
            {
                throw new InvalidOperationException($"{DisplayName} is not started; call StartProcess() first.");
            }

            // await WaitForStartCompletedAsync(timeout, cancellationToken); // Uncomment if the modules need to be more self contained and unreliant on MorpheusEngine class to call this.
            
            var json = JsonSerializer.Serialize(request);
            using var httpRequest = new HttpRequestMessage(HttpMethod.Post, GetModuleUri("/initialize"))
            {
                Content = new StringContent(json, Encoding.UTF8, "application/json")
            };

            using var response = await _httpClient.SendAsync(httpRequest, cancellationToken);
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException(
                    $"{DisplayName} /initialize returned {(int)response.StatusCode} {response.ReasonPhrase}. Body: {body}");
            }
        }

        public async Task WaitForInitializationToCompleteAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            var deadline = DateTime.UtcNow + timeout;
            Exception? lastError = null;

            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();

                // Readiness requires both liveness and initialized=true.
                if (_process.HasExited)
                {
                    throw new InvalidOperationException($"{DisplayName} exited before it became ready.");
                }

                try
                {
                    using var response = await _httpClient.GetAsync(GetModuleUri("/health"), cancellationToken);
                    var body = await response.Content.ReadAsStringAsync(cancellationToken);
                    var health = JsonSerializer.Deserialize<ModuleHealthResponse>(body, HealthJsonOptions);

                    // Terminal failure from modules that accept POST /initialize quickly (e.g. 202) then report bind errors only on /health, or from failed bundled Ollama bootstrap.
                    if (string.Equals(health?.Status, "initialize_failed", StringComparison.OrdinalIgnoreCase)
                        || string.Equals(health?.Status, "ollama_startup_failed", StringComparison.OrdinalIgnoreCase))
                    {
                        throw new TerminalModuleHealthException(
                            $"{DisplayName} reported terminal health status '{health?.Status}' during startup (see module logs). Body: {body}");
                    }

                    if (response.IsSuccessStatusCode)
                    {
                        if (health?.Initialized == true)
                        {
                            return;
                        }

                        lastError = new InvalidOperationException(
                            $"{DisplayName} /health returned 2xx but initialized=false (status={health?.Status ?? "(null)"}).");
                    }
                    else
                    {
                        lastError = new InvalidOperationException(
                            $"{DisplayName} health check returned {(int)response.StatusCode} (status={health?.Status ?? "(null)"}).");
                    }
                }
                catch (TerminalModuleHealthException)
                {
                    throw;
                }
                catch (Exception e)
                {
                    lastError = e;
                }

                await Task.Delay(250, cancellationToken);
            }

            throw new TimeoutException($"Timed out waiting for {DisplayName} to become ready. {lastError?.Message}");
        }

        /// <summary>Host-only escape hatch when cooperative shutdown did not finish in time (e.g. window closed during init).</summary>
        public void ForceKillIfRunning()
        {
            if (!_childProcessSpawned)
            {
                return;
            }

            try
            {
                if (!_process.HasExited)
                {
                    ForceKillProcess();
                }
            }
            finally
            {
                CleanupProcess();
            }
        }

        public async Task StopAsync()
        {
            if (!_childProcessSpawned)
            {
                return;
            }

            if (_process.HasExited)
            {
                CleanupProcess();
                return;
            }

            try
            {
                // Best-effort cooperative stop first.
                using var request = new HttpRequestMessage(HttpMethod.Post, GetModuleUri("/shutdown"));
                request.Content = new ByteArrayContent(Array.Empty<byte>());
                using var response = await _httpClient.SendAsync(request);
                if (!response.IsSuccessStatusCode)
                {
                    Console.WriteLine($"{DisplayName} shutdown endpoint returned {(int)response.StatusCode}.");
                }
            }
            catch (Exception e)
            {
                await Task.Delay(250);
                if (!_process.HasExited)
                {
                    Console.WriteLine($"{DisplayName} graceful shutdown failed: {e.Message}");
                }
            }

            try
            {
                // Hard timebox process exit to keep host shutdown bounded.
                await _process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3));
            }
            catch (TimeoutException)
            {
                Console.WriteLine($"{DisplayName} did not exit in time; terminating process tree.");
                ForceKillProcess();
            }
            catch (Exception e)
            {
                Console.WriteLine($"{DisplayName} encountered error while waiting for shutdown: {e.Message}");
                ForceKillProcess();
            }
            finally
            {
                CleanupProcess();
            }
        }
        #endregion

        #region Private methods
        private ProcessStartInfo CreateProcessStartInfo()
        {
            var artifactPath = ResolveRepositoryRelativePath(_definition.LaunchInfo.Artifact);
            if (!File.Exists(artifactPath))
            {
                throw new FileNotFoundException(
                    $"{DisplayName} artifact not found at '{artifactPath}'. Build the solution first.",
                    artifactPath);
            }

            if (string.Equals(Path.GetExtension(artifactPath), ".dll", StringComparison.OrdinalIgnoreCase))
            {
                return new ProcessStartInfo
                {
                    FileName = "dotnet",
                    Arguments = $"\"{artifactPath}\"",
                    WorkingDirectory = Path.GetDirectoryName(artifactPath) ?? _configuration.GetDotnetRoot(),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
            }

            return new ProcessStartInfo
            {
                FileName = artifactPath,
                WorkingDirectory = Path.GetDirectoryName(artifactPath) ?? _configuration.GetDotnetRoot(),
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
        }

        private void ForwardChildLine(string line, bool isError)
        {
            // Host owns final log shape for child lines; always prepend host module context.
            EngineLog.WriteHostedChildLine(_definition.PortKey, isError, line);
        }

        private string ResolveRepositoryRelativePath(string relativePath) =>
            Path.GetFullPath(Path.Combine(_configuration.RepositoryRoot, relativePath));

        private Uri GetModuleUri(string path) =>
            new($"http://127.0.0.1:{Port}{EngineConfiguration.NormalizePath(path)}");

        private void ForceKillProcess()
        {
            if (_process.HasExited)
            {
                return;
            }

            try
            {
                _process.Kill(true);
                _process.WaitForExit(3000);
            }
            catch (Exception e)
            {
                Console.WriteLine($"{DisplayName} encountered error while stopping: {e.Message}");
            }
        }

        private void CleanupProcess()
        {
            _process.Dispose();
            _process = new Process();
            _childProcessSpawned = false;
        }

        #endregion
    }
}