using System.IO;
using System.Net.Http;
using System.Text;
using System.Windows;

namespace MorpheusEngine.App;

public partial class MainWindow : Window
{
    #region Nested types
    private sealed class UiTextWriter(TextWriter fallback, Action<string> onWrite) : TextWriter
    {
        public override Encoding Encoding => fallback.Encoding;

        public override void Write(char value)
        {
            fallback.Write(value);
            onWrite(value.ToString());
        }

        public override void Write(string? value)
        {
            fallback.Write(value);
            onWrite(value ?? string.Empty);
        }

        public override void WriteLine(string? value)
        {
            fallback.WriteLine(value);
            onWrite((value ?? string.Empty) + Environment.NewLine);
        }
    }
    #endregion

    #region Private data
    private static readonly HttpClient Http = new(); // Used to send HTTP messages from UI.
    private MorpheusEngine _engine = new MorpheusEngine();

    private Task _engineTask = Task.CompletedTask;
    private bool _allowClose = false; // Used to halt window closing while the engine shuts down.
    private bool _shutdownInProgress = false; // Used to guard against duplicate engine shutdown invocations.
    #endregion

    public MainWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Closing += OnClosing;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        Console.OutputEncoding = Encoding.UTF8;
        var uiWriter = new UiTextWriter(Console.Out, AppendLineToPane);
        Console.SetOut(uiWriter);
        Console.SetError(uiWriter);

        Console.WriteLine("MorpheusEngine GUI started.");
        Console.WriteLine("Click Start Engine to run.");
        UpdateButtonState();
    }

    private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (_allowClose) // Shutdown work has already been done, this is a >second invocation.
        {
            Console.SetOut(Console.Out);
            Console.SetError(Console.Error);
            // Don't e.Cancel since it's a valid action.
            return;
        }

        if (_shutdownInProgress) // Guard against duplicate shutdown invocations.
        {
            e.Cancel = true;
            return;
        }

        e.Cancel = true;
        _shutdownInProgress = true;
        UpdateButtonState();

        await StopEngineAsync(isAppClosing: true); // Wait for engine to shut down.

        _allowClose = true; // Authorise window closing now that the engine is shut down.
        _shutdownInProgress = false;
        _ = Dispatcher.BeginInvoke(new Action(Close)); // Queue another close window invocation.
    }

    private async void SendHttpButton_Click(object sender, RoutedEventArgs e)
    {
        var portText = PortTextBox.Text.Trim();
        var endpoint = EndpointTextBox.Text.Trim();

        if (!int.TryParse(portText, out var port) || port < 1 || port > 65535)
        {
            HttpResponsePane.Text = "Invalid port. Enter a number between 1 and 65535.";
            return;
        }

        if (string.IsNullOrWhiteSpace(endpoint))
        {
            endpoint = "/";
        }
        else if (!endpoint.StartsWith('/'))
        {
            endpoint = "/" + endpoint;
        }

        var uri = $"http://127.0.0.1:{port}{endpoint}";
        SendHttpButton.IsEnabled = false;
        HttpResponsePane.Text = $"GET {uri}\r\nSending...";

        try
        {
            using var response = await Http.GetAsync(uri);
            var body = await response.Content.ReadAsStringAsync();
            HttpResponsePane.Text = $"GET {uri}\r\nStatus: {(int)response.StatusCode} {response.ReasonPhrase}\r\n\r\n{body}";
        }
        catch (Exception ex)
        {
            HttpResponsePane.Text = $"GET {uri}\r\nRequest failed:\r\n{ex.Message}";
        }
        finally
        {
            SendHttpButton.IsEnabled = true;
        }
    }

    private void StartEngine()
    {
        if (IsEngineRunning()) return;

        _engine = new MorpheusEngine();

        _engineTask = Task.Run(() =>
        {
            _engine.Run();
            Dispatcher.BeginInvoke(() =>
            {
                _engineTask = Task.CompletedTask;
                UpdateButtonState();
            });
        });

        UpdateButtonState();
    }

    private async Task StopEngineAsync(bool isAppClosing = false)
    {
        if (!IsEngineRunning()) return;

        _engine.RequestShutdown();

        if (IsEngineRunning())
        {
            await Task.WhenAny(_engineTask, Task.Delay(3000));
        }

        _engine = new MorpheusEngine();
        _engineTask = Task.CompletedTask;
        UpdateButtonState();
    }

    private void UpdateButtonState()
    {
        StartButton.IsEnabled = !IsEngineRunning() && !_shutdownInProgress;
        StopButton.IsEnabled = IsEngineRunning() && !_shutdownInProgress;
    }

    #region UI Callbacks
    private void AppendLineToPane(string text)
    {
        Dispatcher.BeginInvoke(() =>
        {
            ConsolePane.AppendText(text);
            ConsolePane.CaretIndex = ConsolePane.Text.Length;
            ConsolePane.ScrollToEnd();
        });
    }
    private void StartButton_Click(object sender, RoutedEventArgs e)
    {
        StartEngine();
    }

    private async void StopButton_Click(object sender, RoutedEventArgs e)
    {
        await StopEngineAsync();
    }
    #endregion

    #region Helpers
    private bool IsEngineRunning()
    {
        return _engineTask != Task.CompletedTask;
    }
    #endregion
}
