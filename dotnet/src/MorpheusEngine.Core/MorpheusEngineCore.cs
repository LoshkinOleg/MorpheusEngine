using System.Threading.Tasks;

namespace MorpheusEngine
{

    public class MorpheusEngine
    {
#region Public data
        public const int MODULE_READY_TIMEOUT_SECONDS = 120;
        public const int MODULE_LISTENING_TIMEOUT_SECONDS = 60;
        public const int MODULE_INITIALIZE_TIMEOUT_SECONDS = 60;
        public readonly EngineConfiguration? Configuration;
        public readonly IReadOnlyList<ManagedModule> Modules;
        public readonly string GameProjectId;
        public readonly string RunId;
        /// <summary>
        /// Completes after each module has been started, host-initialized, and GET /health reports initialized=true (2xx), in engine_config.json list order.
        /// </summary>
        public readonly TaskCompletionSource InitializationCompletedSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
#endregion

        private bool _shutdownRequested = false;
        private CancellationTokenSource? _runShutdownCts = null;

        public MorpheusEngine(string gameProjectId, string runId)
        {
            GameProjectId = gameProjectId.Trim();
            RunId = runId.Trim();
            Configuration = EngineConfigLoader.GetConfiguration();
            var configuration = Configuration ?? throw new InvalidOperationException("Engine configuration is not initialized.");
            Modules = configuration.ModulesInfos
                .Select(module => new ManagedModule(configuration, module))
                .ToArray();
        }

        /// <summary>Cancels in-flight module readiness waits and sets the shutdown flag so <see cref="Run"/> exits its idle loop.</summary>
        public void RequestShutdown()
        {
            _shutdownRequested = true;
            _runShutdownCts?.Cancel();
        }

        /// <summary>Non-graceful teardown of every spawned module process (used when the host must exit before <see cref="Run"/> returns).</summary>
        public void KillChildProcesses()
        {
            foreach (var module in Modules.Reverse())
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
                InitializationCompletedSource.TrySetCanceled(shutdownCts.Token);
            }
            catch (Exception e)
            {
                Console.WriteLine("Engine encoutered error: " + e.Message);
                InitializationCompletedSource.TrySetException(e);
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
            var configuration = Configuration ?? throw new InvalidOperationException("Engine configuration is not initialized.");

            // Run binding happens at engine start (gameProjectId + runId are provided by the UI before boot begins).
            var portsSummary = string.Join(
                ", ",
                configuration.ModulesInfos
                    .OrderBy(m => m.PortKey, StringComparer.OrdinalIgnoreCase)
                    .Select(m => $"{m.PortKey}={configuration.GetRequiredListenPort(m.PortKey)}"));
            Console.WriteLine(
                $"Engine config repo='{configuration.RepositoryRoot}' ports={{ {portsSummary} }} module_aliases={configuration.ModuleAliases.Count} "
                + $"llm_provider_qwen.ollama_port={configuration.LlmProviderOllamaListenPort} default_chat_model='{configuration.LlmProviderDefaultChatModel}' "
                + $"num_ctx={configuration.LlmProviderNumCtx} warmup_game_project_id='{configuration.LlmProviderWarmupGameProjectId}' "
                + $"run.gameProjectId='{GameProjectId}' run.runId='{RunId}'");

            var initializePayload = new InitializeModuleRequest(GameProjectId, RunId);

            // One module at a time: spawn → wait GET /health 2xx + initialized=false → POST /initialize → wait GET /health 2xx + initialized=true.
            foreach (var module in Modules)
            {
                cancellationToken.ThrowIfCancellationRequested();
                Console.WriteLine(
                    $"[Engine] Starting module '{module.DisplayName}' (port_key={module.PortKey}, port={module.Port}, required={module.Required}).");
                module.Start(moduleHostJob);

                module.WaitUntilListeningAsync(TimeSpan.FromSeconds(MODULE_LISTENING_TIMEOUT_SECONDS), cancellationToken).GetAwaiter().GetResult();
                module.InitializeAsync(initializePayload, TimeSpan.FromSeconds(MODULE_INITIALIZE_TIMEOUT_SECONDS), cancellationToken).GetAwaiter().GetResult();
                module.WaitUntilReadyAsync(TimeSpan.FromSeconds(MODULE_READY_TIMEOUT_SECONDS), cancellationToken).GetAwaiter().GetResult();
                Console.WriteLine($"[Engine] Module '{module.DisplayName}' ({module.PortKey}) is healthy.");
            }

            // Signals the GUI (and any other host) that outbound calls may reach a warmed LLM provider.
            InitializationCompletedSource.TrySetResult();

            Console.WriteLine("Engine initialized.");
        }

        private static void Update()
        {
            Thread.Sleep(1);
        }

        private void Shutdown()
        {
            foreach (var module in Modules.Reverse())
            {
                module.StopAsync().GetAwaiter().GetResult();
            }

            Console.WriteLine("Engine shut down.");
        }
    }
}