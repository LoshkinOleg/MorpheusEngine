using System.Diagnostics;
using System.Net;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.TestModuleHost;
using MorpheusEngine.Tests.Integration.Fixtures;
using MorpheusEngine.Tests.Integration.Helpers;
using EngineHost = global::MorpheusEngine.MorpheusEngine;

namespace MorpheusEngine.Tests.Integration.App;

[Collection("EngineProcessState")]
[Trait("Category", "Integration")]
public sealed class MorpheusEngineCoreIntegrationTests : IDisposable
{
    // TestModuleHost sandboxes bind ephemeral OS-assigned loopback ports via GetFreeTcpPort; they deliberately avoid the
    // fixed 59010-59109 integration harness reservation (IntegrationHarnessListenPorts).

    private string? _originalCurrentDirectory;

    // Verifies that startup spawns modules in load order and initializes them after readiness.
    [Fact]
    public async Task MorpheusEngine_StartupSequence_SpawnsModulesInLoadOrder_WaitsForReadiness_AndInitializes()
    {
        using var environment = CreateEnvironment(
            CreateModuleDefinition("llm_provider_qwen", 10),
            CreateModuleDefinition("director", 20),
            CreateModuleDefinition("embeddings_ollama", 30));
        using var runCts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var engine = new EngineHost(environment.GameProjectId, "run_startup");
        var runTask = Task.Run(engine.Run, runCts.Token);

        await engine.InitializationCompletedSource.Task.WaitAsync(TimeSpan.FromSeconds(10), runCts.Token);
        engine.RequestShutdown();
        await runTask.WaitAsync(TimeSpan.FromSeconds(10), runCts.Token);

        ReadEvents(environment.EventLogPath).Should().ContainInOrder(
        [
            "started:router",
            "initialize:router",
            "started:llm_provider_qwen",
            "initialize:llm_provider_qwen",
            "started:director",
            "initialize:director",
            "started:embeddings_ollama",
            "initialize:embeddings_ollama"
        ]);
    }

    // Verifies that a module exiting during bootstrap fails fast before the listen timeout.
    [Fact]
    public async Task MorpheusEngine_ModuleThatExitsDuringBootstrap_FailsFastBeforeListenTimeout()
    {
        using var environment = CreateEnvironment(
            CreateModuleDefinition("llm_provider_qwen", 10, exitBeforeListening: true),
            CreateModuleDefinition("director", 20),
            CreateModuleDefinition("embeddings_ollama", 30));
        var engine = new EngineHost(environment.GameProjectId, "run_crash");
        var runTask = Task.Run(engine.Run);
        var stopwatch = Stopwatch.StartNew();

        Func<Task> act = async () => await engine.InitializationCompletedSource.Task.WaitAsync(TimeSpan.FromSeconds(10));

        await act.Should().ThrowAsync<InvalidOperationException>()
            .WithMessage("*exited before it began listening*");

        stopwatch.Elapsed.Should().BeLessThan(TimeSpan.FromSeconds(5));
        await runTask.WaitAsync(TimeSpan.FromSeconds(10));
    }

    // Verifies that initialization fails when a module reports initialize_failed on health checks.
    [Fact]
    public async Task MorpheusEngine_ModuleReportingInitializeFailedOnHealth_Throws()
    {
        using var environment = CreateEnvironment(
            CreateModuleDefinition("llm_provider_qwen", 10),
            CreateModuleDefinition("director", 20, healthStatusAfterInitialize: "initialize_failed", initializedAfterInitialize: false, healthOkAfterInitialize: false, initializeResponseStatusCode: 202),
            CreateModuleDefinition("embeddings_ollama", 30));
        var engine = new EngineHost(environment.GameProjectId, "run_initialize_failed");
        var runTask = Task.Run(engine.Run);

        Func<Task> act = async () => await engine.InitializationCompletedSource.Task.WaitAsync(TimeSpan.FromSeconds(10));

        await act.Should().ThrowAsync<InvalidOperationException>()
            .WithMessage("*initialize_failed*");
        await runTask.WaitAsync(TimeSpan.FromSeconds(10));
    }

    // Verifies that shutdown notifies every module and force-kills stragglers.
    [Fact]
    public async Task MorpheusEngine_Shutdown_SendsShutdownToAllModules_AndForceKillsStragglers()
    {
        using var environment = CreateEnvironment(
            CreateModuleDefinition("llm_provider_qwen", 10),
            CreateModuleDefinition("director", 20, ignoreShutdown: true, writePidFile: true),
            CreateModuleDefinition("embeddings_ollama", 30));
        using var runCts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var engine = new EngineHost(environment.GameProjectId, "run_shutdown");
        var runTask = Task.Run(engine.Run, runCts.Token);

        await engine.InitializationCompletedSource.Task.WaitAsync(TimeSpan.FromSeconds(10), runCts.Token);
        engine.RequestShutdown();
        await runTask.WaitAsync(TimeSpan.FromSeconds(10), runCts.Token);

        ReadEvents(environment.EventLogPath).Should().Contain("shutdown:llm_provider_qwen");
        ReadEvents(environment.EventLogPath).Should().Contain("shutdown:director");
        ReadEvents(environment.EventLogPath).Should().Contain("shutdown:embeddings_ollama");
        ReadEvents(environment.EventLogPath).Should().Contain("shutdown:router");

        var stubbornPid = int.Parse(File.ReadAllText(environment.ModulePidPaths["director"]));
        ProcessExists(stubbornPid).Should().BeFalse();
    }

    // Verifies that the Windows job object prevents orphan child processes.
    [Fact]
    public async Task WindowsJobObject_OnWindows_PreventsOrphanProcesses()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        using var sandbox = new TempDirectory();
        var port = GetFreeTcpPort();
        var childPidPath = Path.Combine(sandbox.Path, "orphan-child.pid");
        var eventLogPath = Path.Combine(sandbox.Path, "events.log");
        var moduleRoot = Path.Combine(sandbox.Path, "job-module");
        var moduleDll = ProvisionModuleHost(
            moduleRoot,
            new ModuleDefinition(
                "job_host",
                10,
                SpawnOrphanChildOnStart: true,
                ChildPidFilePath: childPidPath,
                EventLogPath: eventLogPath,
                PortOverride: port));

        var process = Process.Start(new ProcessStartInfo
        {
            FileName = "dotnet",
            Arguments = $"\"{moduleDll}\"",
            WorkingDirectory = moduleRoot,
            UseShellExecute = false,
            CreateNoWindow = true
        }) ?? throw new InvalidOperationException("Failed to start test module host.");

        try
        {
            using var job = new WindowsJobObject();
            job.AssignProcess(process);
            await WaitForFileAsync(childPidPath, TimeSpan.FromSeconds(5));

            var childPid = int.Parse(File.ReadAllText(childPidPath));
            ProcessExists(process.Id).Should().BeTrue();
            ProcessExists(childPid).Should().BeTrue();

            job.Dispose();
            await WaitForProcessExitAsync(process.Id, TimeSpan.FromSeconds(5));
            await WaitForProcessExitAsync(childPid, TimeSpan.FromSeconds(5));

            ProcessExists(childPid).Should().BeFalse();
        }
        finally
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(3000);
            }

            process.Dispose();
        }
    }

    public void Dispose()
    {
        EngineConfigLoader.ResetForTesting();
        if (_originalCurrentDirectory is not null)
        {
            Environment.CurrentDirectory = _originalCurrentDirectory;
            _originalCurrentDirectory = null;
        }
    }

    private TestEnvironment CreateEnvironment(params ModuleDefinition[] definitions)
    {
        var effectiveDefinitions = new List<ModuleDefinition>
        {
            CreateModuleDefinition("router", 5)
        };
        effectiveDefinitions.AddRange(definitions);

        var gameProjectId = "test_game";
        var eventLogPath = Path.Combine(Path.GetTempPath(), "morpheus_engine_events_" + Guid.NewGuid().ToString("N") + ".log");
        File.WriteAllText(eventLogPath, string.Empty);

        var moduleRoots = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var moduleArtifacts = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var modulePidPaths = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var modulePorts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var finalizedDefinitions = new List<ModuleDefinition>();
        foreach (var definition in effectiveDefinitions)
        {
            var port = definition.PortOverride ?? GetFreeTcpPort();
            var moduleRoot = Path.Combine(Path.GetTempPath(), $"morpheus_module_{definition.PortKey}_{Guid.NewGuid():N}");
            moduleRoots[definition.PortKey] = moduleRoot;
            modulePorts[definition.PortKey] = port;
            modulePidPaths[definition.PortKey] = Path.Combine(moduleRoot, "module.pid");
            var finalizedDefinition = definition with
            {
                EventLogPath = eventLogPath,
                WritePidFile = definition.WritePidFile || definition.IgnoreShutdown,
                PortOverride = port
            };
            finalizedDefinitions.Add(finalizedDefinition);
            moduleArtifacts[definition.PortKey] = ProvisionModuleHost(
                moduleRoot,
                finalizedDefinition);
        }

        var gameProject = new TempGameProject(
            gameProjectId,
            BuildManifestJson(),
            loreCsv: null,
            systemInstructions: null,
            engineConfigJson: BuildEngineConfigJson(finalizedDefinitions, modulePorts, moduleArtifacts));
        _originalCurrentDirectory = Environment.CurrentDirectory;
        Environment.CurrentDirectory = gameProject.RepositoryRoot;
        EngineConfigLoader.ResetForTesting();
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(gameProject.RepositoryRoot);
        return new TestEnvironment(gameProject, eventLogPath, moduleRoots, modulePidPaths);
    }

    private static string BuildManifestJson()
    {
        var manifest = new
        {
            id = "test_game",
            title = "Test Game",
            required_modules = Array.Empty<string>(),
            turn_pipeline = "test_pipeline"
        };
        return JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true });
    }

    private static string BuildEngineConfigJson(
        IReadOnlyList<ModuleDefinition> definitions,
        IReadOnlyDictionary<string, int> ports,
        IReadOnlyDictionary<string, string> artifacts)
    {
        var moduleMap = definitions.ToDictionary(definition => definition.PortKey, StringComparer.OrdinalIgnoreCase);
        var config = new
        {
            module_aliases = new Dictionary<string, string>
            {
                ["generic_llm_provider"] = "llm_provider_qwen",
                ["generic_director"] = "director",
                ["generic_embeddings"] = "embeddings_ollama"
            },
            turn_pipelines = new Dictionary<string, object>
            {
                ["test_pipeline"] = new
                {
                    steps = new object[]
                    {
                        new
                        {
                            id = "director_init_probe",
                            target_module = "generic_director",
                            path = "/initialize",
                            method = "POST",
                            body_template = "{}"
                        }
                    },
                    response_mapping = new
                    {
                        source_step = "director_init_probe",
                        type = "director_message_response"
                    }
                }
            },
            modules = new object[]
            {
                new
                {
                    port_key = "router",
                    port = ports["router"],
                    load_order = moduleMap["router"].LoadOrder,
                    display_name = "Router",
                    required_by_engine = true,
                    launch = artifacts["router"],
                    endpoints = StandardEndpoints()
                },
                new
                {
                    port_key = "llm_provider_qwen",
                    port = ports["llm_provider_qwen"],
                    load_order = moduleMap["llm_provider_qwen"].LoadOrder,
                    display_name = "LLM Provider",
                    required_by_engine = true,
                    launch = artifacts["llm_provider_qwen"],
                    num_ctx = 4096,
                    ollama_port = 19112,
                    default_chat_model = "fake-qwen",
                    endpoints = StandardEndpoints()
                },
                new
                {
                    port_key = "director",
                    port = ports["director"],
                    load_order = moduleMap["director"].LoadOrder,
                    display_name = "Director",
                    required_by_engine = true,
                    launch = artifacts["director"],
                    endpoints = StandardEndpoints()
                },
                new
                {
                    port_key = "embeddings_ollama",
                    port = ports["embeddings_ollama"],
                    load_order = moduleMap["embeddings_ollama"].LoadOrder,
                    display_name = "Embeddings",
                    required_by_engine = true,
                    launch = artifacts["embeddings_ollama"],
                    ollama_port = 19112,
                    default_embedding_model = "fake-embeddings",
                    keep_model_loaded_for = "30m",
                    embeddings_num_ctx = 1024,
                    endpoints = StandardEndpoints()
                }
            }
        };

        return JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true });
    }

    private static object[] StandardEndpoints()
    {
        return
        [
            new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
            new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
            new { path = "/shutdown", description = "Shutdown", method = "POST", template_contracts_id = "module_shutdown" }
        ];
    }

    private static ModuleDefinition CreateModuleDefinition(
        string portKey,
        int loadOrder,
        bool exitBeforeListening = false,
        string healthStatusAfterInitialize = "healthy",
        bool initializedAfterInitialize = true,
        bool healthOkAfterInitialize = true,
        int initializeResponseStatusCode = 200,
        bool ignoreShutdown = false,
        bool writePidFile = false,
        bool spawnOrphanChildOnStart = false,
        string? childPidFilePath = null,
        string? eventLogPath = null,
        int? portOverride = null)
    {
        return new ModuleDefinition(
            portKey,
            loadOrder,
            exitBeforeListening,
            healthStatusAfterInitialize,
            initializedAfterInitialize,
            healthOkAfterInitialize,
            initializeResponseStatusCode,
            ignoreShutdown,
            writePidFile,
            spawnOrphanChildOnStart,
            childPidFilePath,
            eventLogPath,
            portOverride);
    }

    private static string ProvisionModuleHost(string moduleRoot, ModuleDefinition definition)
    {
        Directory.CreateDirectory(moduleRoot);
        var helperDirectory = Path.GetDirectoryName(typeof(TestModuleHostMarker).Assembly.Location)
            ?? throw new InvalidOperationException("Could not resolve helper output directory.");
        foreach (var sourceFile in Directory.GetFiles(helperDirectory))
        {
            var destination = Path.Combine(moduleRoot, Path.GetFileName(sourceFile));
            File.Copy(sourceFile, destination, overwrite: true);
        }

        var behavior = new
        {
            Port = definition.PortOverride ?? throw new InvalidOperationException("PortOverride must be set before provisioning."),
            ModuleName = definition.PortKey,
            EventLogPath = definition.EventLogPath ?? throw new InvalidOperationException("EventLogPath must be set before provisioning."),
            PidFilePath = definition.WritePidFile ? Path.Combine(moduleRoot, "module.pid") : null,
            ExitBeforeListening = definition.ExitBeforeListening,
            ExitCode = 1,
            InitialHealthStatus = "awaiting_initialize",
            InitialHealthOk = false,
            InitializeResponseStatusCode = definition.InitializeResponseStatusCode,
            HealthStatusAfterInitialize = definition.HealthStatusAfterInitialize,
            HealthOkAfterInitialize = definition.HealthOkAfterInitialize,
            InitializedAfterInitialize = definition.InitializedAfterInitialize,
            IgnoreShutdown = definition.IgnoreShutdown,
            SpawnOrphanChildOnStart = definition.SpawnOrphanChildOnStart,
            ChildPidFilePath = definition.ChildPidFilePath
        };
        File.WriteAllText(
            Path.Combine(moduleRoot, "behavior.json"),
            JsonSerializer.Serialize(behavior, new JsonSerializerOptions { WriteIndented = true }));

        return Path.Combine(moduleRoot, "MorpheusEngine.TestModuleHost.dll");
    }

    private static IReadOnlyList<string> ReadEvents(string path)
    {
        return File.ReadAllLines(path)
            .Where(line => !string.IsNullOrWhiteSpace(line))
            .ToArray();
    }

    private static bool ProcessExists(int pid)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private static async Task WaitForProcessExitAsync(int pid, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (!ProcessExists(pid))
            {
                return;
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"Process {pid} did not exit in time.");
    }

    private static async Task WaitForFileAsync(string path, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(path))
            {
                return;
            }

            await Task.Delay(50);
        }

        throw new TimeoutException($"File '{path}' was not created in time.");
    }

    private static int GetFreeTcpPort()
    {
        using var listener = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private sealed record ModuleDefinition(
        string PortKey,
        int LoadOrder,
        bool ExitBeforeListening = false,
        string HealthStatusAfterInitialize = "healthy",
        bool InitializedAfterInitialize = true,
        bool HealthOkAfterInitialize = true,
        int InitializeResponseStatusCode = 200,
        bool IgnoreShutdown = false,
        bool WritePidFile = false,
        bool SpawnOrphanChildOnStart = false,
        string? ChildPidFilePath = null,
        string? EventLogPath = null,
        int? PortOverride = null);

    private sealed class TestEnvironment : IDisposable
    {
        private readonly TempGameProject _gameProject;
        private readonly IReadOnlyDictionary<string, string> _moduleRoots;

        public TestEnvironment(
            TempGameProject gameProject,
            string eventLogPath,
            IReadOnlyDictionary<string, string> moduleRoots,
            IReadOnlyDictionary<string, string> modulePidPaths)
        {
            _gameProject = gameProject;
            EventLogPath = eventLogPath;
            _moduleRoots = moduleRoots;
            ModulePidPaths = modulePidPaths;
        }

        public string GameProjectId => _gameProject.GameProjectId;

        public string EventLogPath { get; }

        public IReadOnlyDictionary<string, string> ModulePidPaths { get; }

        public void Dispose()
        {
            // TestEnvironment manages out-of-process module subprocesses spawned by the engine
            // under test; their lifetime is the engine's responsibility, not the harness's. The
            // OS often hasn't finalized handles on the temp tree by the time this Dispose runs,
            // so we keep the same best-effort semantics here that File.Delete(EventLogPath) and
            // moduleRoot deletion already use below. The harness teardown audit
            // (docs/LLM_TestHarnessAudit.md, lines 3-54) targets in-process listener teardown,
            // not these spawned-process cleanup paths.
            try
            {
                _gameProject.Dispose();
            }
            catch
            {
            }

            try
            {
                if (File.Exists(EventLogPath))
                {
                    File.Delete(EventLogPath);
                }
            }
            catch
            {
            }

            foreach (var moduleRoot in _moduleRoots.Values)
            {
                try
                {
                    if (Directory.Exists(moduleRoot))
                    {
                        Directory.Delete(moduleRoot, recursive: true);
                    }
                }
                catch
                {
                }
            }
        }
    }

    private sealed class TempDirectory : IDisposable
    {
        public TempDirectory()
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "morpheus_temp_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path);
        }

        public string Path { get; }

        public void Dispose()
        {
            try
            {
                if (Directory.Exists(Path))
                {
                    Directory.Delete(Path, recursive: true);
                }
            }
            catch
            {
            }
        }
    }
}
