namespace MorpheusEngine
{

    public class MorpheusEngine
    {
        private bool _shutdownRequested;
        private readonly RouterModule _routerModule = new();
        private readonly LlmProviderQwenModule _llmProviderQwenModule = new();

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
            _routerModule.Run();
            _llmProviderQwenModule.Run();

            Console.WriteLine("Engine initialized.");
        }

        private static void Update()
        {
            Thread.Sleep(1);
        }

        private void Shutdown()
        {
            _llmProviderQwenModule.Stop();
            _routerModule.Stop();
            Console.WriteLine("Engine shut down.");
        }
    }
}