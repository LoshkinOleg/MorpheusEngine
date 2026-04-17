using MorpheusEngine;

var director = new Director();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    director.RequestShutdown();
};

await director.Run();
