using MorpheusEngine;

var provider = new LlmProviderQwen();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    provider.RequestShutdown();
};

await provider.Run();
