using System.Windows;

namespace MorpheusEngine.App;

public static class Program
{
    [STAThread] // WPF specific tag.
    public static void Main()
    {
        var app = new Application();
        app.Run(new MainWindow());
    }
}