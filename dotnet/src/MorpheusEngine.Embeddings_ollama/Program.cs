using MorpheusEngine;

var module = new EmbeddingsOllamaModule();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    module.RequestShutdown();
};

await module.RunAsync();
