using System.Threading.Tasks;

namespace MorpheusEngine
{

    public class MorpheusEngine
    {
        private bool _shutdownRequested;
        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
        private readonly IReadOnlyList<ManagedModule> _modules;

        /// <summary>
        /// Completes after required module /health checks succeed (including LLM provider Ollama warm-up); fails if initialization throws.
        /// </summary>
        private readonly TaskCompletionSource _initializationCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public MorpheusEngine()
        {
            _modules = _configuration.ModulesInfos
                .Select(module => new ManagedModule(_configuration, module))
                .ToArray();
        }

        /// <summary>Observes successful <see cref="Initialize"/> completion; use from UI to avoid treating the engine as playable during boot.</summary>
        public Task InitializationCompleted => _initializationCompleted.Task;

        public void RequestShutdown() => _shutdownRequested = true;

        public void Run()
        {
            try
            {
                Initialize();
                while (!_shutdownRequested)
                {
                    Update();
                }
            }
            catch (Exception e)
            {
                Console.WriteLine("Engine encoutered error: " + e.Message);
                _initializationCompleted.TrySetException(e);
            }
            finally
            {
                Shutdown();
            }
        }

        private void Initialize()
        {
            foreach (var module in _modules)
            {
                module.Start();
            }

            // Required modules include llm_provider_qwen, which does not open /health until Ollama is up and the warm-up POST has finished.
            var readinessTasks = _modules
                .Where(module => module.Required)
                .Select(module => module.WaitUntilReadyAsync(TimeSpan.FromSeconds(120)));

            Task.WhenAll(readinessTasks).GetAwaiter().GetResult();

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