using System.Collections.ObjectModel;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using MorpheusEngine;

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

    private sealed record HttpCallResult(
        string Method,
        string Uri,
        int StatusCode,
        string ReasonPhrase,
        string Body);

    private sealed record GameChatMessage(
        string SpeakerLabel,
        string Text,
        HorizontalAlignment BubbleAlignment,
        Brush BubbleBackground,
        Brush BubbleBorderBrush,
        Brush BubbleForeground);
    #endregion

    #region Private data
    private readonly EngineConfiguration? _config;
    private readonly string? _configLoadError;
    private readonly ObservableCollection<GameChatMessage> _gameMessages = [];
    private static readonly HttpClient Http = new();
    private MorpheusEngine? _engine;

    private Task _engineTask = Task.CompletedTask;
    private bool _allowClose;
    private bool _shutdownInProgress;
    private bool _suppressEndpointPresetEvents;
    private bool _applyingEndpointFromPreset;
    private bool _gameRequestInFlight;
    /// <summary>Logical game project folder under game_projects/ (mirrors TS layout). Must match an on-disk project; no silent fallback.</summary>
    private string _gameProjectId = "sandcrawler";
    /// <summary>Per-run id; empty until <see cref="EnsureRunStartedAsync"/> succeeds.</summary>
    private string _runId = string.Empty;
    /// <summary>Next turn index to send (1-based; must match MAX(snapshots.turn)+1).</summary>
    private int _nextTurn = 1;
    private string[] _qwenMonitorModuleNames = ["Qwen", "LlmProvider_qwen"];
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };
    #endregion

    public MainWindow()
    {
        InitializeComponent();
        try
        {
            _config = EngineConfigLoader.GetConfiguration();
        }
        catch (EngineConfigurationException ex)
        {
            _configLoadError = ex.Message;
        }

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

        if (_configLoadError is not null)
        {
            MessageBox.Show(
                $"Engine configuration failed to load:\n{_configLoadError}",
                "MorpheusEngine",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
        }

        if (_config is not null)
        {
            _qwenMonitorModuleNames = _config.ModulesInfos
                .Where(module => module.PortKey.Equals("llm_provider_qwen", StringComparison.OrdinalIgnoreCase))
                .Select(module => module.DisplayName)
                .Append("Qwen")
                .Append("LlmProvider_qwen")
                .Where(name => !string.IsNullOrWhiteSpace(name))
                .Distinct(StringComparer.Ordinal)
                .ToArray();
        }

        GameMessagesListBox.ItemsSource = _gameMessages;
        AppendSystemGameMessage(
            "Game tab ready. First send starts a run (router POST /initialize → session_store), then each message goes to router /turn with sequencing and SQLite persistence.");
        SetGameStatus(
            _config is null
                ? "Fix engine_config.json and restart the application."
                : "Ready to send player input through the router.",
            isError: _config is null);

        PopulatePortComboBox();
        PopulateEndpointPresetComboBox();
        ApplyEndpointBodyTemplateIfNeeded();
        UpdateButtonState();
    }

    private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (_allowClose)
        {
            Console.SetOut(Console.Out);
            Console.SetError(Console.Error);
            return;
        }

        if (_shutdownInProgress)
        {
            e.Cancel = true;
            return;
        }

        e.Cancel = true;
        _shutdownInProgress = true;
        UpdateButtonState();

        await StopEngineAsync();

        _allowClose = true;
        _shutdownInProgress = false;
        _ = Dispatcher.BeginInvoke(new Action(Close));
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

        if (_config is null)
        {
            HttpResponsePane.Text = "Engine configuration is not loaded. Fix engine_config.json and restart.";
            return;
        }

        SendHttpButton.IsEnabled = false;

        try
        {
            var previewEndpoint = EngineConfiguration.NormalizePath(endpoint);
            var previewUri = $"http://127.0.0.1:{port}{previewEndpoint}";
            HttpResponsePane.Text = $"{previewUri}\r\nSending...";

            var result = await SendRequestAsync(port, endpoint, requestBody);
            HttpResponsePane.Text =
                $"{result.Method} {result.Uri}\r\nStatus: {result.StatusCode} {result.ReasonPhrase}\r\n\r\n{result.Body}";
        }
        catch (Exception ex)
        {
            var safeEndpoint = EngineConfiguration.NormalizePath(endpoint);
            HttpResponsePane.Text = $"http://127.0.0.1:{port}{safeEndpoint}\r\nRequest failed:\r\n{ex.Message}";
        }
        finally
        {
            SendHttpButton.IsEnabled = true;
        }
    }

    private async void GameSendButton_Click(object sender, RoutedEventArgs e)
    {
        await SubmitGameInputAsync();
    }

    private async void GameInputTextBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter || Keyboard.Modifiers != ModifierKeys.None)
        {
            return;
        }

        e.Handled = true;
        await SubmitGameInputAsync();
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

    private void EndpointTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_applyingEndpointFromPreset)
        {
            return;
        }

        ApplyEndpointBodyTemplateIfNeeded();
        TrySelectMatchingPreset();
    }

    private void PortComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        PopulateEndpointPresetComboBox();
        ApplyEndpointBodyTemplateIfNeeded();
    }

    private void CustomPortTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        PopulateEndpointPresetComboBox();
        ApplyEndpointBodyTemplateIfNeeded();
    }

    private void EndpointPresetComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressEndpointPresetEvents)
        {
            return;
        }

        if (EndpointPresetComboBox.SelectedItem is not ComboBoxItem item || item.Tag is not EngineEndpointInfo info)
        {
            return;
        }

        _applyingEndpointFromPreset = true;
        try
        {
            EndpointTextBox.Text = info.Path;
            HttpRequestBodyTextBox.Text = info.BodyTemplate ?? string.Empty;
            HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
        }
        finally
        {
            _applyingEndpointFromPreset = false;
        }
    }

    private void StartEngine()
    {
        if (IsEngineRunning())
        {
            return;
        }

        if (_config is null)
        {
            MessageBox.Show(
                _configLoadError is not null
                    ? $"Cannot start engine:\n{_configLoadError}"
                    : "Engine configuration is not loaded.",
                "MorpheusEngine",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        var engine = new MorpheusEngine();
        _engine = engine;

        _engineTask = Task.Run(() =>
        {
            engine.Run();
            Dispatcher.BeginInvoke(() =>
            {
                _engineTask = Task.CompletedTask;
                UpdateButtonState();
            });
        });

        UpdateButtonState();
    }

    private async Task StopEngineAsync()
    {
        if (!IsEngineRunning())
        {
            return;
        }

        _engine?.RequestShutdown();

        if (IsEngineRunning())
        {
            await Task.WhenAny(_engineTask, Task.Delay(3000));
        }

        _engine = null;
        _engineTask = Task.CompletedTask;
        UpdateButtonState();
    }

    private void UpdateButtonState()
    {
        StartButton.IsEnabled = !IsEngineRunning() && !_shutdownInProgress && _config is not null;
        StopButton.IsEnabled = IsEngineRunning() && !_shutdownInProgress;

        if (GameSendButton is not null)
        {
            GameSendButton.IsEnabled = !_shutdownInProgress && !_gameRequestInFlight;
        }

        if (GameInputTextBox is not null)
        {
            GameInputTextBox.IsEnabled = !_shutdownInProgress && !_gameRequestInFlight;
        }
    }

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

    private void PopulatePortComboBox()
    {
        if (PortComboBox is null || _config is null)
        {
            return;
        }

        PortComboBox.Items.Clear();
        foreach (var module in _config.ModulesInfos)
        {
            var port = _config.GetRequiredListenPort(module.PortKey);

            PortComboBox.Items.Add(new ComboBoxItem
            {
                Content = $"{module.DisplayName} ({port})",
                Tag = port.ToString()
            });
        }

        if (PortComboBox.Items.Count > 0)
        {
            PortComboBox.SelectedIndex = 0;
        }
    }

    private void PopulateEndpointPresetComboBox()
    {
        if (EndpointPresetComboBox is null || EndpointTextBox is null)
        {
            return;
        }

        _suppressEndpointPresetEvents = true;
        try
        {
            EndpointPresetComboBox.Items.Clear();
            EndpointPresetComboBox.Items.Add(new ComboBoxItem
            {
                Content = "(Custom - use Endpoint field)",
                Tag = null
            });

            if (_config is not null && int.TryParse(GetEffectivePortText(), out var port))
            {
                var module = _config.GetModuleForListeningPort(port);
                if (module is not null)
                {
                    foreach (var endpoint in module.Endpoints)
                    {
                        var label = string.IsNullOrEmpty(endpoint.Description)
                            ? endpoint.Path
                            : $"{endpoint.Description} - {endpoint.Path}";
                        EndpointPresetComboBox.Items.Add(new ComboBoxItem
                        {
                            Content = label,
                            Tag = endpoint
                        });
                    }
                }
            }

            var path = EngineConfiguration.NormalizePath(EndpointTextBox.Text);
            var selectIndex = 0;
            for (var i = 1; i < EndpointPresetComboBox.Items.Count; i++)
            {
                if (EndpointPresetComboBox.Items[i] is ComboBoxItem comboBoxItem
                    && comboBoxItem.Tag is EngineEndpointInfo endpoint
                    && string.Equals(
                        EngineConfiguration.NormalizePath(endpoint.Path),
                        path,
                        StringComparison.OrdinalIgnoreCase))
                {
                    selectIndex = i;
                    break;
                }
            }

            EndpointPresetComboBox.SelectedIndex = selectIndex;
        }
        finally
        {
            _suppressEndpointPresetEvents = false;
        }
    }

    private void TrySelectMatchingPreset()
    {
        if (_suppressEndpointPresetEvents || EndpointPresetComboBox is null || EndpointTextBox is null)
        {
            return;
        }

        var path = EngineConfiguration.NormalizePath(EndpointTextBox.Text);
        var selectIndex = 0;
        for (var i = 1; i < EndpointPresetComboBox.Items.Count; i++)
        {
            if (EndpointPresetComboBox.Items[i] is ComboBoxItem comboBoxItem
                && comboBoxItem.Tag is EngineEndpointInfo endpoint
                && string.Equals(
                    EngineConfiguration.NormalizePath(endpoint.Path),
                    path,
                    StringComparison.OrdinalIgnoreCase))
            {
                selectIndex = i;
                break;
            }
        }

        if (EndpointPresetComboBox.SelectedIndex == selectIndex)
        {
            return;
        }

        _suppressEndpointPresetEvents = true;
        try
        {
            EndpointPresetComboBox.SelectedIndex = selectIndex;
        }
        finally
        {
            _suppressEndpointPresetEvents = false;
        }
    }

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

    private async Task<HttpCallResult> SendRequestAsync(
        int port,
        string endpoint,
        string? requestBody,
        string? forcedMethod = null)
    {
        if (_config is null)
        {
            throw new InvalidOperationException("Engine configuration is not loaded.");
        }

        var normalizedEndpoint = EngineConfiguration.NormalizePath(endpoint);
        var endpointInfo = _config.FindEndpointForPort(port, normalizedEndpoint);
        var usePost = forcedMethod is not null
            ? forcedMethod.Equals("POST", StringComparison.OrdinalIgnoreCase)
            : endpointInfo is not null
                ? endpointInfo.Method.Equals("POST", StringComparison.OrdinalIgnoreCase)
                : !string.IsNullOrWhiteSpace(requestBody);
        var method = usePost ? "POST" : "GET";
        var uri = $"http://127.0.0.1:{port}{normalizedEndpoint}";

        using var request = new HttpRequestMessage(
            usePost ? HttpMethod.Post : HttpMethod.Get,
            uri);
        if (usePost)
        {
            request.Content = string.IsNullOrWhiteSpace(requestBody)
                ? new ByteArrayContent(Array.Empty<byte>())
                : new StringContent(requestBody, Encoding.UTF8, "application/json");
        }

        using var response = await Http.SendAsync(request);
        var body = await response.Content.ReadAsStringAsync();
        return new HttpCallResult(
            method,
            uri,
            (int)response.StatusCode,
            response.ReasonPhrase ?? string.Empty,
            body);
    }

    /// <summary>POST router /initialize once per UI session so world_state.db exists before the first /turn.</summary>
    private async Task<bool> EnsureRunStartedAsync()
    {
        if (_config is null)
        {
            return false;
        }

        if (!string.IsNullOrEmpty(_runId))
        {
            return true;
        }

        _runId = Guid.NewGuid().ToString("D");
        var startBody = JsonSerializer.Serialize(new InitializeModuleRequest(_gameProjectId, _runId), JsonOptions);
        var startResult = await SendRequestAsync(_config.GetRequiredListenPort("router"), "/initialize", startBody, "POST");
        if (startResult.StatusCode is not (>= 200 and < 300))
        {
            AppendSystemGameMessage(
                $"Router /initialize returned {startResult.StatusCode} {startResult.ReasonPhrase}.\n{startResult.Body}");
            _runId = string.Empty;
            return false;
        }

        _nextTurn = 1;
        return true;
    }

    private async Task SubmitGameInputAsync()
    {
        if (_gameRequestInFlight)
        {
            return;
        }

        if (_config is null)
        {
            SetGameStatus("Engine configuration is not loaded. Fix engine_config.json and restart.", isError: true);
            return;
        }

        var playerInput = GameInputTextBox.Text;
        if (string.IsNullOrWhiteSpace(playerInput))
        {
            SetGameStatus("Enter a player action before sending.", isError: true);
            return;
        }

        AppendPlayerGameMessage(playerInput.Trim());
        GameInputTextBox.Clear();
        _gameRequestInFlight = true;
        SetGameStatus("Preparing run / sending player input to router /turn...");
        UpdateButtonState();

        try
        {
            if (!await EnsureRunStartedAsync())
            {
                SetGameStatus("Failed to start run via router /initialize.", isError: true);
                return;
            }

            var body = JsonSerializer.Serialize(
                new TurnRequest(_runId, _gameProjectId, _nextTurn, playerInput.Trim()),
                JsonOptions);
            var result = await SendRequestAsync(_config.GetRequiredListenPort("router"), "/turn", body, "POST");

            if (result.StatusCode is >= 200 and < 300)
            {
                if (TryParseIntentResponse(result.Body, out var intentResponse))
                {
                    AppendEngineGameMessage(FormatIntentResponse(intentResponse));
                    SetGameStatus($"Turn {_nextTurn}: intent {intentResponse.Intent} (persisted to session DB).");
                    _nextTurn++;
                }
                else
                {
                    AppendEngineGameMessage(result.Body);
                    SetGameStatus("Router /turn responded with non-standard JSON. Showing raw response.");
                }
            }
            else
            {
                AppendSystemGameMessage($"Router /turn returned {result.StatusCode} {result.ReasonPhrase}.\n{result.Body}");
                SetGameStatus($"Router /turn failed with {result.StatusCode} {result.ReasonPhrase}.", isError: true);
            }
        }
        catch (Exception e)
        {
            AppendSystemGameMessage("Request failed: " + e.Message);
            SetGameStatus("Game request failed.", isError: true);
        }
        finally
        {
            _gameRequestInFlight = false;
            UpdateButtonState();
            GameInputTextBox.Focus();
        }
    }

    private static bool TryParseIntentResponse(string body, out IntentResponse intentResponse)
    {
        intentResponse = null!;

        try
        {
            var parsed = JsonSerializer.Deserialize<IntentResponse>(body, JsonOptions);
            if (parsed is null)
            {
                return false;
            }

            intentResponse = parsed;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static string FormatIntentResponse(IntentResponse response)
    {
        var lines = new List<string> { $"Intent: {response.Intent}" };
        if (response.Parameters.Count == 0)
        {
            lines.Add("Params: (none)");
        }
        else
        {
            lines.Add("Params:");
            foreach (var parameter in response.Parameters)
            {
                lines.Add($"- {parameter.Key}: {parameter.Value}");
            }
        }

        return string.Join(Environment.NewLine, lines);
    }

    private void SetGameStatus(string text, bool isError = false)
    {
        GameStatusTextBlock.Text = text;
        GameStatusTextBlock.Foreground = isError
            ? Brushes.Salmon
            : Brushes.LightSteelBlue;
    }

    private void AppendPlayerGameMessage(string text) =>
        AppendGameMessage("You", text, HorizontalAlignment.Right, "#365CA8", "#4C74C2", Brushes.White);

    private void AppendEngineGameMessage(string text) =>
        AppendGameMessage("Engine", text, HorizontalAlignment.Left, "#1E2837", "#304057", Brushes.WhiteSmoke);

    private void AppendSystemGameMessage(string text) =>
        AppendGameMessage("System", text, HorizontalAlignment.Left, "#302534", "#5B456D", Brushes.WhiteSmoke);

    private void AppendGameMessage(
        string speaker,
        string text,
        HorizontalAlignment alignment,
        string backgroundColor,
        string borderColor,
        Brush foreground)
    {
        _gameMessages.Add(new GameChatMessage(
            speaker,
            text,
            alignment,
            (Brush)new BrushConverter().ConvertFromString(backgroundColor)!,
            (Brush)new BrushConverter().ConvertFromString(borderColor)!,
            foreground));

        if (_gameMessages.Count > 0)
        {
            GameMessagesListBox.ScrollIntoView(_gameMessages[^1]);
        }
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

    private bool TryExtractQwenLogLine(string text, out string qwenLogLine)
    {
        foreach (var moduleName in _qwenMonitorModuleNames)
        {
            var normalPrefix = $"[{moduleName}] OLLAMA_IO ";
            if (text.StartsWith(normalPrefix, StringComparison.Ordinal))
            {
                qwenLogLine = text.Substring(normalPrefix.Length).TrimEnd('\r', '\n');
                return true;
            }

            var errorPrefix = $"[{moduleName}:ERR] OLLAMA_IO ";
            if (text.StartsWith(errorPrefix, StringComparison.Ordinal))
            {
                qwenLogLine = "ERR: " + text.Substring(errorPrefix.Length).TrimEnd('\r', '\n');
                return true;
            }
        }

        qwenLogLine = string.Empty;
        return false;
    }

    private void ApplyEndpointBodyTemplateIfNeeded()
    {
        if (_applyingEndpointFromPreset || HttpRequestBodyTextBox is null || EndpointTextBox is null)
        {
            return;
        }

        if (!string.IsNullOrWhiteSpace(HttpRequestBodyTextBox.Text))
        {
            return;
        }

        if (_config is null || !int.TryParse(GetEffectivePortText(), out var port))
        {
            return;
        }

        var match = _config.FindEndpointForPort(port, EndpointTextBox.Text);
        if (match?.BodyTemplate is null)
        {
            return;
        }

        HttpRequestBodyTextBox.Text = match.BodyTemplate;
        HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
    }

    private bool IsEngineRunning() => _engineTask != Task.CompletedTask;
}
