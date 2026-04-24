using MorpheusEngine;

EngineLog.Initialize("SessionStore");

var host = new SessionStoreHost();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    host.RequestShutdown();
};

await host.RunAsync();
