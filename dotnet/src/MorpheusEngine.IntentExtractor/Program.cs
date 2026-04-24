using MorpheusEngine;

EngineLog.Initialize("IntentExtractor");

var intentExtractor = new IntentExtractor();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    intentExtractor.RequestShutdown();
};

await intentExtractor.Run();
