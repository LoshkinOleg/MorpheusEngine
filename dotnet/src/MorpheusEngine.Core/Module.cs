using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;

namespace MorpheusEngine
{
    public sealed class ManagedModule
    {
        private static readonly HttpClient Http = new()
        {
            Timeout = TimeSpan.FromSeconds(3)
        };

        private readonly EngineConfiguration _configuration;
        private readonly EngineModuleInfo _definition;
        private Process _process = new Process();

        public ManagedModule(EngineConfiguration configuration, EngineModuleInfo definition)
        {
            _configuration = configuration;
            _definition = definition;
            Port = _configuration.ResolvePort(definition.PortKey)
                ?? throw new InvalidOperationException($"Unknown port key '{definition.PortKey}'.");
        }

        public string DisplayName => _definition.DisplayName;
        public string PortKey => _definition.PortKey;
        public int Port { get; }
        public bool Required => _definition.Required;

        public void Start()
        {
            var psi = CreateProcessStartInfo();
            _process = Process.Start(psi) ?? throw new InvalidOperationException($"Failed to start {DisplayName}.");
            _process.OutputDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine($"[{DisplayName}] " + e.Data);
                }
            };
            _process.ErrorDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine($"[{DisplayName}:ERR] " + e.Data);
                }
            };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
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
                        return;
                    }

                    lastError = new InvalidOperationException($"{DisplayName} health check returned {(int)response.StatusCode}.");
                }
                catch (Exception e)
                {
                    lastError = e;
                }

                await Task.Delay(250, cancellationToken);
            }

            throw new TimeoutException($"Timed out waiting for {DisplayName} to become ready. {lastError?.Message}");
        }

        public async Task StopAsync()
        {
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

            if (useDevLaunch && !string.IsNullOrWhiteSpace(_definition.Launch.DevProject))
            {
                var projectPath = ResolveRepositoryRelativePath(_definition.Launch.DevProject);
                return new ProcessStartInfo
                {
                    FileName = "dotnet",
                    Arguments = $"run --project \"{projectPath}\" --",
                    WorkingDirectory = _configuration.DotnetRoot,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
            }

            var artifactPath = ResolveRepositoryRelativePath(_definition.Launch.Artifact);
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
                    WorkingDirectory = Path.GetDirectoryName(artifactPath) ?? _configuration.DotnetRoot,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
            }

            return new ProcessStartInfo
            {
                FileName = artifactPath,
                WorkingDirectory = Path.GetDirectoryName(artifactPath) ?? _configuration.DotnetRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
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
        }
    }
}