using MorpheusEngine;

var director = new MemoryDirector();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    director.RequestShutdown();
};

await director.RunAsync();
