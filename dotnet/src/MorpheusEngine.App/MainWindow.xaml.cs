using System.IO;
using System.Net.Http;
using System.Text;
using System.Windows;
using System.Windows.Controls;

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
    private const int QWEN_PORT = 8791;
    private const int ROUTER_PORT = 8790;
    private const string GENERATE_BODY_TEMPLATE =
        "{\n" +
        "  \"prompt\": \"Write a short response.\",\n" +
        "  \"model\": \"qwen2.5:7b-instruct\",\n" +
        "  \"system\": \"You are a helpful assistant.\"\n" +
        "}";
    private const string TURN_BODY_TEMPLATE =
        "{\n" +
        "  \"playerInput\": \"look around\"\n" +
        "}";
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
        ApplyEndpointBodyTemplateIfNeeded();
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
        var portText = GetEffectivePortText();
        var endpoint = EndpointTextBox.Text.Trim();
        var requestBody = HttpRequestBodyTextBox.Text;

        if (!int.TryParse(portText, out var port) || port < 1 || port > 65535)
        {
            HttpResponsePane.Text = "Invalid port. Enter a custom port (1-65535) or choose one from the list.";
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
        var hasBody = !string.IsNullOrWhiteSpace(requestBody);
        var method = hasBody ? "POST" : "GET";

        SendHttpButton.IsEnabled = false;
        HttpResponsePane.Text = $"{method} {uri}\r\nSending...";

        try
        {
            using var response = hasBody
                ? await Http.PostAsync(uri, new StringContent(requestBody, Encoding.UTF8, "application/json"))
                : await Http.GetAsync(uri);
            var body = await response.Content.ReadAsStringAsync();
            HttpResponsePane.Text = $"{method} {uri}\r\nStatus: {(int)response.StatusCode} {response.ReasonPhrase}\r\n\r\n{body}";
        }
        catch (Exception ex)
        {
            HttpResponsePane.Text = $"{method} {uri}\r\nRequest failed:\r\n{ex.Message}";
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

            if (TryExtractQwenLogLine(text, out var qwenLogLine))
            {
                AppendQwenMonitorEntry(qwenLogLine);
            }
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

    private void ClearQwenMonitorButton_Click(object sender, RoutedEventArgs e)
    {
        QwenMonitorPane.Clear();
    }

    private void EndpointTextBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
    {
        ApplyEndpointBodyTemplateIfNeeded();
    }

    private void PortComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        ApplyEndpointBodyTemplateIfNeeded();
    }
    #endregion

    #region Helpers
    private string GetSelectedPortText()
    {
        if (PortComboBox.SelectedItem is ComboBoxItem item && item.Tag is string tag)
        {
            return tag;
        }

        return string.Empty;
    }

    private string GetEffectivePortText()
    {
        var customPort = CustomPortTextBox.Text.Trim();
        if (!string.IsNullOrWhiteSpace(customPort))
        {
            return customPort;
        }

        return GetSelectedPortText();
    }

    private void AppendQwenMonitorEntry(string text)
    {
        Dispatcher.BeginInvoke(() =>
        {
            var timestamp = DateTime.Now.ToString("HH:mm:ss.fff");
            QwenMonitorPane.AppendText($"[{timestamp}] {text}{Environment.NewLine}----------------{Environment.NewLine}");
            QwenMonitorPane.CaretIndex = QwenMonitorPane.Text.Length;
            QwenMonitorPane.ScrollToEnd();
        });
    }

    private static bool TryExtractQwenLogLine(string text, out string qwenLogLine)
    {
        if (text.StartsWith("[Qwen] OLLAMA_IO ", StringComparison.Ordinal))
        {
            qwenLogLine = text.Substring("[Qwen] OLLAMA_IO ".Length).TrimEnd('\r', '\n');
            return true;
        }

        if (text.StartsWith("[Qwen:ERR] OLLAMA_IO ", StringComparison.Ordinal))
        {
            qwenLogLine = "ERR: " + text.Substring("[Qwen:ERR] OLLAMA_IO ".Length).TrimEnd('\r', '\n');
            return true;
        }

        qwenLogLine = string.Empty;
        return false;
    }

    private void ApplyEndpointBodyTemplateIfNeeded()
    {
        if (HttpRequestBodyTextBox is null || EndpointTextBox is null)
        {
            return;
        }

        if (!string.IsNullOrWhiteSpace(HttpRequestBodyTextBox.Text))
        {
            return;
        }

        var endpoint = EndpointTextBox.Text;
        if (IsGenerateEndpoint(endpoint))
        {
            HttpRequestBodyTextBox.Text = GENERATE_BODY_TEMPLATE;
            HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
            return;
        }

        if (IsTurnEndpoint(endpoint) && IsRouterSelected())
        {
            HttpRequestBodyTextBox.Text = TURN_BODY_TEMPLATE;
            HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
        }
    }

    private static bool IsGenerateEndpoint(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
        {
            return false;
        }

        var normalized = endpoint.Trim();
        if (!normalized.StartsWith('/'))
        {
            normalized = "/" + normalized;
        }

        return normalized.Equals("/generate", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsTurnEndpoint(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
        {
            return false;
        }

        var normalized = endpoint.Trim();
        if (!normalized.StartsWith('/'))
        {
            normalized = "/" + normalized;
        }

        return normalized.Equals("/turn", StringComparison.OrdinalIgnoreCase);
    }

    private bool IsRouterSelected()
    {
        if (PortComboBox is null)
        {
            return false;
        }

        return int.TryParse(GetSelectedPortText(), out var selectedPort) && selectedPort == ROUTER_PORT;
    }

    private bool IsEngineRunning()
    {
        return _engineTask != Task.CompletedTask;
    }
    #endregion
}
