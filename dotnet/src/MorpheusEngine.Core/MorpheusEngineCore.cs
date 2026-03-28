namespace MorpheusEngine
{

    public class MorpheusEngine
    {
        private bool _shutdownRequested;
        private readonly EngineConfiguration _configuration = EngineConfigLoader.GetConfiguration();
        private readonly IReadOnlyList<ManagedModule> _modules;

        public MorpheusEngine()
        {
            _modules = _configuration.Modules
                .Select(module => new ManagedModule(_configuration, module))
                .ToArray();
        }

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

            var readinessTasks = _modules
                .Where(module => module.Required)
                .Select(module => module.WaitUntilReadyAsync(TimeSpan.FromSeconds(15)));

            Task.WhenAll(readinessTasks).GetAwaiter().GetResult();

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