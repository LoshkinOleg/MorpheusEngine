using MorpheusEngine;

Router router = new Router();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;              // don’t kill the process immediately
    router.RequestShutdown();     // exit the while loop; finally → Shutdown()
};
await router.Run();