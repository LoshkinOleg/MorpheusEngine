using System.Diagnostics;
using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public sealed class ManagedModule
    {
        private static readonly HttpClient Http = new()
        {
            Timeout = TimeSpan.FromSeconds(3)
        };

        private static readonly JsonSerializerOptions HealthJsonOptions = new()
        {
            PropertyNameCaseInsensitive = true
        };

        private readonly EngineConfiguration _configuration;
        private readonly EngineModuleInfo _definition;
        private Process _process = new Process();

        /// <summary>True after <see cref="Start"/> successfully spawned a child; <see cref="StopAsync"/> is a no-op until then.</summary>
        private bool _childProcessSpawned = false;

        public ManagedModule(EngineConfiguration configuration, EngineModuleInfo definition)
        {
            _configuration = configuration;
            _definition = definition;
            Port = _configuration.GetRequiredListenPort(definition.PortKey);
        }

        public string DisplayName => _definition.DisplayName;
        public string PortKey => _definition.PortKey;
        public int Port { get; }
        public bool Required => _definition.Required;

        /// <param name="moduleHostJob">When non-null (Windows host), the spawned module process is assigned so it cannot outlive the job handle.</param>
        public void Start(WindowsJobObject? moduleHostJob)
        {
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
        /// </summary>
        public async Task WaitUntilListeningAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            var deadline = DateTime.UtcNow + timeout;
            Exception? lastError = null;

            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();

                if (_process.HasExited)
                {
                    throw new InvalidOperationException($"{DisplayName} exited before it began listening.");
                }

                try
                {
                    using var response = await Http.GetAsync(GetModuleUri("/health"), cancellationToken);
                    if (!response.IsSuccessStatusCode)
                    {
                        lastError = new InvalidOperationException($"{DisplayName} /health returned {(int)response.StatusCode}.");
                    }
                    else
                    {
                        var body = await response.Content.ReadAsStringAsync(cancellationToken);
                        var health = JsonSerializer.Deserialize<ModuleHealthResponse>(body, HealthJsonOptions);
                        if (health is not null && !health.Initialized)
                        {
                            return;
                        }

                        lastError = new InvalidOperationException(
                            $"{DisplayName} /health returned 2xx but missing or unexpected body (expected initialized=false).");
                    }
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
        /// Host-driven run binding: waits for GET /health (pre-initialize 2xx), then <see cref="PostInitializeAndBindRunAsync"/>.
        /// </summary>
        public async Task InitializeAsync(
            InitializeModuleRequest request,
            TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            if (!_childProcessSpawned)
            {
                throw new InvalidOperationException($"{DisplayName} is not started; call Start() first.");
            }

            await WaitUntilListeningAsync(timeout, cancellationToken);
            await PostInitializeAndBindRunAsync(request, cancellationToken);
        }

        /// <summary>POST /initialize with <see cref="InitializeModuleRequest"/> JSON; throws if non-success.</summary>
        private async Task PostInitializeAndBindRunAsync(InitializeModuleRequest request, CancellationToken cancellationToken)
        {
            var json = JsonSerializer.Serialize(request);
            using var httpRequest = new HttpRequestMessage(HttpMethod.Post, GetModuleUri("/initialize"))
            {
                Content = new StringContent(json, Encoding.UTF8, "application/json")
            };

            using var response = await Http.SendAsync(httpRequest, cancellationToken);
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException(
                    $"{DisplayName} /initialize returned {(int)response.StatusCode} {response.ReasonPhrase}. Body: {body}");
            }
        }

        public async Task PostJsonEnsureSuccessAsync(string absolutePath, string jsonBody, CancellationToken cancellationToken = default)
        {
            if (!_childProcessSpawned)
            {
                throw new InvalidOperationException($"{DisplayName} is not started; call Start() first.");
            }

            if (string.IsNullOrWhiteSpace(absolutePath))
            {
                throw new ArgumentException("absolutePath must be non-empty.", nameof(absolutePath));
            }

            var normalized = EngineConfiguration.NormalizePath(absolutePath);
            using var httpRequest = new HttpRequestMessage(HttpMethod.Post, GetModuleUri(normalized))
            {
                Content = string.IsNullOrWhiteSpace(jsonBody)
                    ? new ByteArrayContent(Array.Empty<byte>())
                    : new StringContent(jsonBody, Encoding.UTF8, "application/json")
            };

            using var response = await Http.SendAsync(httpRequest, cancellationToken);
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException(
                    $"{DisplayName} {normalized} returned {(int)response.StatusCode} {response.ReasonPhrase}. Body: {body}");
            }
        }

        public async Task WaitUntilReadyAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            var deadline = DateTime.UtcNow + timeout;
            Exception? lastError = null;

            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();

                if (_process.HasExited)
                {
                    throw new InvalidOperationException($"{DisplayName} exited before it became ready.");
                }

                try
                {
                    using var response = await Http.GetAsync(GetModuleUri("/health"), cancellationToken);
                    if (response.IsSuccessStatusCode)
                    {
                        var body = await response.Content.ReadAsStringAsync(cancellationToken);
                        var health = JsonSerializer.Deserialize<ModuleHealthResponse>(body, HealthJsonOptions);
                        if (health?.Initialized == true)
                        {
                            return;
                        }

                        lastError = new InvalidOperationException(
                            $"{DisplayName} /health returned 2xx but initialized=false (status={health?.Status ?? "(null)"}).");
                    }
                    else
                    {
                        lastError = new InvalidOperationException($"{DisplayName} health check returned {(int)response.StatusCode}.");
                    }
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
                using var request = new HttpRequestMessage(HttpMethod.Post, GetModuleUri("/shutdown"));
                request.Content = new ByteArrayContent(Array.Empty<byte>());
                using var response = await Http.SendAsync(request);
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

        private ProcessStartInfo CreateProcessStartInfo()
        {
            var useDevLaunch = string.Equals(Environment.GetEnvironmentVariable("MORPHEUS_DEV_LAUNCH"), "1", StringComparison.OrdinalIgnoreCase)
                || string.Equals(Environment.GetEnvironmentVariable("MORPHEUS_DEV_LAUNCH"), "true", StringComparison.OrdinalIgnoreCase);

            if (useDevLaunch && !string.IsNullOrWhiteSpace(_definition.LaunchInfo.DevProject))
            {
                var projectPath = ResolveRepositoryRelativePath(_definition.LaunchInfo.DevProject);
                return new ProcessStartInfo
                {
                    FileName = "dotnet",
                    Arguments = $"run --project \"{projectPath}\" --",
                    WorkingDirectory = _configuration.GetDotnetRoot(),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
            }

            var artifactPath = ResolveRepositoryRelativePath(_definition.LaunchInfo.Artifact);
            if (!File.Exists(artifactPath))
            {
                throw new FileNotFoundException(
                    $"{DisplayName} artifact not found at '{artifactPath}'. Build the solution first or set MORPHEUS_DEV_LAUNCH=1.",
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
            // Force-env: child kept local EngineLog prefixes; relay unchanged.
            if (ShouldRelayChildLinesVerbatim())
            {
                EngineLog.WriteForwardedLine(line);
                return;
            }

            // Some lines (e.g. router turn markers) are already fully prefixed in the child so console and llmLog share one id.
            if (LooksLikePrefixedUnifiedLogLine(line))
            {
                EngineLog.WriteForwardedLine(line);
                return;
            }

            EngineLog.WriteHostedChildLine(_definition.PortKey, isError, line);
        }

        private static bool ShouldRelayChildLinesVerbatim() =>
            string.Equals(Environment.GetEnvironmentVariable(EngineLog.ForceChildPrefixEnvVar), "1", StringComparison.OrdinalIgnoreCase)
            || string.Equals(Environment.GetEnvironmentVariable(EngineLog.ForceChildPrefixEnvVar), "true", StringComparison.OrdinalIgnoreCase);

        /// <summary>Detects lines emitted with <see cref="EngineLog.FormatLinePrefix"/> in the child (already include id and session time).</summary>
        private static bool LooksLikePrefixedUnifiedLogLine(string line)
        {
            var t = line.TrimStart();
            return t.StartsWith("[", StringComparison.Ordinal)
                && t.Contains(" ; ", StringComparison.Ordinal)
                && t.Contains("] [", StringComparison.Ordinal);
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
    }
}