using System.Text.Json;
using System.Threading.Tasks;

namespace MorpheusEngine
{

    public class MorpheusEngine
    {
        private bool _shutdownRequested;
        private CancellationTokenSource? _runShutdownCts;
        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
        private readonly IReadOnlyList<ManagedModule> _modules;
        private readonly InitializeModuleRequest _runRequest;

        /// <summary>
        /// Completes after each module has been started, host-initialized, and has reported /health success, in engine_config.json list order.
        /// </summary>
        private readonly TaskCompletionSource _initializationCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);

        private const int ModuleReadyTimeoutSeconds = 120;
        private const int ModuleListeningTimeoutSeconds = 60;
        private const int ModuleInitializeTimeoutSeconds = 60;

        public MorpheusEngine(string gameProjectId, string runId)
        {
            GameRunLogPaths.RequireSafePathSegment(nameof(gameProjectId), gameProjectId);
            GameRunLogPaths.RequireSafePathSegment(nameof(runId), runId);

            _runRequest = new InitializeModuleRequest(gameProjectId.Trim(), runId.Trim());
            _modules = _configuration.ModulesInfos
                .Select(module => new ManagedModule(_configuration, module))
                .ToArray();
        }

        /// <summary>Observes successful <see cref="Initialize"/> completion; use from UI to avoid treating the engine as playable during boot.</summary>
        public Task InitializationCompleted => _initializationCompleted.Task;

        /// <summary>Cancels in-flight module readiness waits and sets the shutdown flag so <see cref="Run"/> exits its idle loop.</summary>
        public void RequestShutdown()
        {
            _shutdownRequested = true;
            _runShutdownCts?.Cancel();
        }

        /// <summary>Non-graceful teardown of every spawned module process (used when the host must exit before <see cref="Run"/> returns).</summary>
        public void KillChildProcesses()
        {
            foreach (var module in _modules.Reverse())
            {
                module.ForceKillIfRunning();
            }
        }

        public void Run()
        {
            using var shutdownCts = new CancellationTokenSource();
            _runShutdownCts = shutdownCts;
            WindowsJobObject? moduleProcessJob = null;
            if (OperatingSystem.IsWindows())
            {
                moduleProcessJob = new WindowsJobObject();
                Console.WriteLine("[Engine] Module host job object active (processes terminate when the host releases the job).");
            }

            try
            {
                Initialize(shutdownCts.Token, moduleProcessJob);
                while (!_shutdownRequested)
                {
                    Update();
                }
            }
            catch (OperationCanceledException oce) when (shutdownCts.Token.IsCancellationRequested || _shutdownRequested)
            {
                Console.WriteLine("Engine run cancelled (shutdown requested during startup or idle): " + oce.Message);
                _initializationCompleted.TrySetCanceled(shutdownCts.Token);
            }
            catch (Exception e)
            {
                Console.WriteLine("Engine encoutered error: " + e.Message);
                _initializationCompleted.TrySetException(e);
            }
            finally
            {
                _runShutdownCts = null;
                Shutdown();
                moduleProcessJob?.Dispose();
            }
        }

        private void Initialize(CancellationToken cancellationToken, WindowsJobObject? moduleHostJob)
        {
            // Run binding happens at engine start (gameProjectId + runId are provided by the UI before boot begins).
            // One-time configuration summary: modules read config independently, but the host log should stay concise.
            var portsSummary = string.Join(
                ", ",
                _configuration.ModulesInfos
                    .OrderBy(m => m.PortKey, StringComparer.OrdinalIgnoreCase)
                    .Select(m => $"{m.PortKey}={_configuration.GetRequiredListenPort(m.PortKey)}"));
            Console.WriteLine(
                $"Engine config repo='{_configuration.RepositoryRoot}' ports={{ {portsSummary} }} module_aliases={_configuration.ModuleAliases.Count} "
                + $"llm_provider_qwen.ollama_port={_configuration.LlmProviderOllamaListenPort} default_chat_model='{_configuration.LlmProviderDefaultChatModel}' "
                + $"num_ctx={_configuration.LlmProviderNumCtx} warmup_game_project_id='{_configuration.LlmProviderWarmupGameProjectId}' "
                + $"run.gameProjectId='{_runRequest.GameProjectId}' run.runId='{_runRequest.RunId}'");

            // Bind host-side file logging once per run before module initialization so forwarded lines get unified ids immediately.
            EngineFileLogActivation.ActivatePrimary(_configuration.RepositoryRoot, _runRequest.GameProjectId, _runRequest.RunId);
            var requestJson = JsonSerializer.Serialize(_runRequest);

            // One module at a time: spawn → wait listener → activate log dir → bind run → wait /health OK.
            foreach (var module in _modules)
            {
                cancellationToken.ThrowIfCancellationRequested();
                Console.WriteLine(
                    $"[Engine] Starting module '{module.DisplayName}' (port_key={module.PortKey}, port={module.Port}, required={module.Required}).");
                module.Start(moduleHostJob);

                module.WaitUntilListeningAsync(TimeSpan.FromSeconds(ModuleListeningTimeoutSeconds), cancellationToken).GetAwaiter().GetResult();
                module.PostJsonEnsureSuccessAsync("/engine_log/activate", requestJson, cancellationToken).GetAwaiter().GetResult();
                module.InitializeAsync(_runRequest, TimeSpan.FromSeconds(ModuleInitializeTimeoutSeconds), cancellationToken).GetAwaiter().GetResult();
                module.WaitUntilReadyAsync(TimeSpan.FromSeconds(ModuleReadyTimeoutSeconds), cancellationToken).GetAwaiter().GetResult();
                Console.WriteLine($"[Engine] Module '{module.DisplayName}' ({module.PortKey}) is healthy.");
            }

            // Signals the GUI (and any other host) that outbound calls may reach a warmed LLM provider.
            _initializationCompleted.TrySetResult();

            Console.WriteLine("Engine initialized.");
        }

        private static void Update()
        {
            Thread.Sleep(1);
        }

        private void Shutdown()
        {
            foreach (var module in _modules.Reverse())
            {
                module.StopAsync().GetAwaiter().GetResult();
            }

            Console.WriteLine("Engine shut down.");
        }
    }
}