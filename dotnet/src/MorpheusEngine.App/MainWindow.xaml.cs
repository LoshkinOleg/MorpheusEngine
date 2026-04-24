using System.Collections.ObjectModel;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
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
        string SpeakerHeading,
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

    /// <summary>Background task executing <see cref="MorpheusEngine.Run"/>; null when idle (never replace with a sentinel Task).</summary>
    private Task? _engineRunTask = null;

    private const int EngineStopGraceSeconds = 45;
    private const int EngineStopKillFollowupSeconds = 15;

    /// <summary>True only after required modules (including warmed LLM provider) report healthy; avoids sending game traffic during boot.</summary>
    private bool _engineModulesReadyForGame = false;
    private bool _allowClose;
    private bool _shutdownInProgress;
    private bool _suppressEndpointPresetEvents;
    private bool _applyingEndpointFromPreset;
    private bool _gameRequestInFlight;
    /// <summary>Logical game project folder under game_projects/ (mirrors TS layout). Must match an on-disk project; no silent fallback.</summary>
    private string _gameProjectId = "sandcrawler";
    /// <summary>Per-run id; set when the engine is started (run binding happens at engine start).</summary>
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

        // Install per-line prefixes into the same stream the UI captures.
        EngineLog.Initialize("App");

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

        GameMessagesItemsControl.ItemsSource = _gameMessages;
        AppendSystemGameMessage(string.Empty);
        RefreshGameTurnHeader();
        SetGameStatus(
            _config is null
                ? "Fix engine_config.json and restart the application."
                : string.Empty,
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

    private void CopyConsoleToClipboardButton_Click(object sender, RoutedEventArgs e)
    {
        CopyTextToClipboardOrWarn(ConsolePane.Text, "Console");
    }

    private void CopyQwenMonitorToClipboardButton_Click(object sender, RoutedEventArgs e)
    {
        CopyTextToClipboardOrWarn(QwenMonitorPane.Text, "Qwen Monitor");
    }

    /// <summary>Writes the full pane text to the clipboard; surfaces failures so the user is not left guessing.</summary>
    private static void CopyTextToClipboardOrWarn(string text, string paneDisplayName)
    {
        try
        {
            Clipboard.SetText(text);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Could not copy {paneDisplayName} to the clipboard:\n{ex.Message}",
                "MorpheusEngine",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
        }
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

        var projectIdText = (GameProjectIdTextBox?.Text ?? string.Empty).Trim();
        if (string.IsNullOrWhiteSpace(projectIdText))
        {
            MessageBox.Show(
                "Game project id cannot be empty.",
                "MorpheusEngine",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        _gameProjectId = projectIdText;
        _runId = Guid.NewGuid().ToString("D");
        _nextTurn = 1;
        RefreshGameTurnHeader();

        var engine = new MorpheusEngine(_gameProjectId, _runId);
        _engine = engine;
        _engineModulesReadyForGame = false;

        _engineRunTask = Task.Run(() =>
        {
            try
            {
                engine.Run();
            }
                finally
                {
                    Dispatcher.BeginInvoke(() =>
                    {
                        _engineRunTask = null;
                        _engineModulesReadyForGame = false;
                        UpdateButtonState();
                    });
                }
        });

        _ = ObserveEngineInitializationAsync(engine);

        UpdateButtonState();
    }

    private async Task ObserveEngineInitializationAsync(MorpheusEngine engine)
    {
        try
        {
            await engine.InitializationCompletedSource.Task.ConfigureAwait(false);
            await Dispatcher.InvokeAsync(() =>
            {
                // Ignore completion if the user stopped this run before we got back to the UI thread.
                if (!ReferenceEquals(_engine, engine))
                {
                    return;
                }

                _engineModulesReadyForGame = true;
                QwenMonitorPane.Clear();
                UpdateButtonState();
            });
        }
        catch
        {
            await Dispatcher.InvokeAsync(() =>
            {
                if (!ReferenceEquals(_engine, engine))
                {
                    return;
                }

                _engineModulesReadyForGame = false;
                UpdateButtonState();
            });
        }
    }

    private async Task StopEngineAsync()
    {
        var runTask = _engineRunTask;
        if (runTask is null && _engine is null)
        {
            return;
        }

        _engineModulesReadyForGame = false;
        var engineRef = _engine;
        engineRef?.RequestShutdown();

        if (runTask is not null)
        {
            var grace = TimeSpan.FromSeconds(EngineStopGraceSeconds);
            await Task.WhenAny(runTask, Task.Delay(grace)).ConfigureAwait(false);
            if (!runTask.IsCompleted && engineRef is not null)
            {
                Console.WriteLine(
                    $"[App] Engine did not stop within {EngineStopGraceSeconds}s; killing child module processes.");
                engineRef.KillChildProcesses();
                await Task.WhenAny(runTask, Task.Delay(TimeSpan.FromSeconds(EngineStopKillFollowupSeconds)))
                    .ConfigureAwait(false);
            }

            try
            {
                await runTask.ConfigureAwait(false);
            }
            catch (Exception e)
            {
                Console.WriteLine("[App] Engine task completed with error: " + e.Message);
            }
        }

        await Dispatcher.InvokeAsync(() =>
        {
            _engine = null;
            _runId = string.Empty;
            _nextTurn = 1;
            RefreshGameTurnHeader();
            UpdateButtonState();
        });
    }

    private void UpdateButtonState()
    {
        var running = IsEngineRunning();
        StartButton.IsEnabled = !running && !_shutdownInProgress && _config is not null;
        StopButton.IsEnabled = running && !_shutdownInProgress;

        if (EngineStatusEllipse is not null)
        {
            // Running: green; stopped: dim gray (fail loud in UI, no ambiguous yellow).
            EngineStatusEllipse.Fill = running
                ? new SolidColorBrush(Color.FromRgb(61, 204, 119))
                : new SolidColorBrush(Color.FromRgb(90, 95, 106));
            EngineStatusEllipse.ToolTip = running ? "Engine running" : "Engine stopped";
        }

        if (GameChatInteractionRoot is not null)
        {
            // Transcript + composer stay inactive until required modules (including LLM warm-up) are ready.
            GameChatInteractionRoot.IsEnabled = running && _engineModulesReadyForGame;
        }

        if (GameSendButton is not null)
        {
            GameSendButton.IsEnabled = running && _engineModulesReadyForGame && !_shutdownInProgress && !_gameRequestInFlight;
        }

        if (GameInputTextBox is not null)
        {
            GameInputTextBox.IsEnabled = running && _engineModulesReadyForGame && !_shutdownInProgress && !_gameRequestInFlight;
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

        AppendPlayerGameMessage(playerInput.Trim(), _nextTurn);
        GameInputTextBox.Clear();
        _gameRequestInFlight = true;
        SetGameStatus("Sending player input to router /turn...");
        UpdateButtonState();

        try
        {
            if (string.IsNullOrWhiteSpace(_runId))
            {
                SetGameStatus("Engine run is not bound. Start the engine and wait for it to become ready.", isError: true);
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
                    AppendEngineGameMessage(FormatIntentResponse(intentResponse), _nextTurn);
                    SetGameStatus(string.Empty);
                    _nextTurn++;
                    RefreshGameTurnHeader();
                }
                else
                {
                    AppendEngineGameMessage(result.Body, _nextTurn);
                    SetGameStatus(string.Empty);
                    _nextTurn++;
                    RefreshGameTurnHeader();
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
        // Avoid a blank status row consuming vertical space when there is nothing to show.
        GameStatusTextBlock.Visibility = string.IsNullOrWhiteSpace(text) && !isError
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private void AppendPlayerGameMessage(string text, int turnNumber) =>
        AppendGameMessage("You", turnNumber, text, HorizontalAlignment.Right, "#365CA8", "#4C74C2", Brushes.White);

    private void AppendEngineGameMessage(string text, int turnNumber) =>
        AppendGameMessage("Engine", turnNumber, text, HorizontalAlignment.Left, "#1E2837", "#304057", Brushes.WhiteSmoke);

    private void AppendSystemGameMessage(string text) =>
        AppendGameMessage("System", null, text, HorizontalAlignment.Left, "#302534", "#5B456D", Brushes.WhiteSmoke);

    private static string BuildSpeakerHeading(string speaker, int? turnNumber) =>
        turnNumber is int t ? $"{speaker} · Turn {t}" : speaker;

    private void RefreshGameTurnHeader()
    {
        if (GameTurnHeaderTextBlock is null)
        {
            return;
        }

        GameTurnHeaderTextBlock.Text = $"Current turn: {_nextTurn}";
    }

    private void AppendGameMessage(
        string speaker,
        int? turnNumber,
        string text,
        HorizontalAlignment alignment,
        string backgroundColor,
        string borderColor,
        Brush foreground)
    {
        _gameMessages.Add(new GameChatMessage(
            BuildSpeakerHeading(speaker, turnNumber),
            text,
            alignment,
            (Brush)new BrushConverter().ConvertFromString(backgroundColor)!,
            (Brush)new BrushConverter().ConvertFromString(borderColor)!,
            foreground));

        ScheduleScrollGameMessagesToEnd();
    }

    private void ScheduleScrollGameMessagesToEnd()
    {
        // Defer until layout so ExtentHeight reflects the new item; pixel scroll avoids clipped tails from item-based scrolling.
        _ = Dispatcher.BeginInvoke(
            () =>
            {
                GameMessagesScrollViewer.UpdateLayout();
                var maxOffset = Math.Max(0, GameMessagesScrollViewer.ExtentHeight - GameMessagesScrollViewer.ViewportHeight);
                GameMessagesScrollViewer.ScrollToVerticalOffset(maxOffset);
            },
            DispatcherPriority.Loaded);
    }

    private void AppendQwenMonitorEntry(string text)
    {
        Dispatcher.BeginInvoke(() =>
        {
            QwenMonitorPane.AppendText(text + Environment.NewLine);
            QwenMonitorPane.CaretIndex = QwenMonitorPane.Text.Length;
            QwenMonitorPane.ScrollToEnd();
        });
    }

    private bool TryExtractQwenLogLine(string text, out string qwenLogLine)
    {
        text = text.TrimEnd('\r', '\n');

        foreach (var moduleName in _qwenMonitorModuleNames)
        {
            var normalPrefix = $"[{moduleName}] OLLAMA_IO ";
            if (text.StartsWith(normalPrefix, StringComparison.Ordinal))
            {
                qwenLogLine = text.Substring(normalPrefix.Length);
                return true;
            }

            var errorPrefix = $"[{moduleName}:ERR] OLLAMA_IO ";
            if (text.StartsWith(errorPrefix, StringComparison.Ordinal))
            {
                qwenLogLine = "ERR: " + text.Substring(errorPrefix.Length);
                return true;
            }

            // New EngineLog prefix: [entryId ; HH:MM:SS::cc] [LlmProvider_qwen] OLLAMA_IO ...
            var newNormalMarker = $"] [{moduleName}] OLLAMA_IO ";
            var newNormalIdx = text.IndexOf(newNormalMarker, StringComparison.Ordinal);
            if (newNormalIdx >= 0)
            {
                qwenLogLine = text.Substring(newNormalIdx + newNormalMarker.Length);
                return true;
            }

            var newErrMarker = $"] [{moduleName}:ERR] OLLAMA_IO ";
            var newErrIdx = text.IndexOf(newErrMarker, StringComparison.Ordinal);
            if (newErrIdx >= 0)
            {
                qwenLogLine = "ERR: " + text.Substring(newErrIdx + newErrMarker.Length);
                return true;
            }

            // Legacy EngineLog: +00012.34s #000123 [LlmProvider_qwen] OLLAMA_IO ...
            if (text.StartsWith("+", StringComparison.Ordinal))
            {
                var tagStart = text.IndexOf('[', StringComparison.Ordinal);
                if (tagStart >= 0)
                {
                    var tagEnd = text.IndexOf(']', tagStart + 1);
                    if (tagEnd > tagStart)
                    {
                        var tag = text.Substring(tagStart + 1, tagEnd - tagStart - 1);
                        if (string.Equals(tag, moduleName, StringComparison.Ordinal)
                            || string.Equals(tag, moduleName + ":ERR", StringComparison.Ordinal))
                        {
                            var afterTag = tagEnd + 1;
                            if (afterTag < text.Length && text[afterTag] == ' ')
                            {
                                afterTag++;
                            }

                            const string ollamaPrefix = "OLLAMA_IO ";
                            if (afterTag + ollamaPrefix.Length <= text.Length
                                && string.Equals(text.Substring(afterTag, ollamaPrefix.Length), ollamaPrefix, StringComparison.Ordinal))
                            {
                                var payload = text.Substring(afterTag + ollamaPrefix.Length);
                                qwenLogLine = tag.EndsWith(":ERR", StringComparison.Ordinal) ? "ERR: " + payload : payload;
                                return true;
                            }
                        }
                    }
                }
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

    /// <summary>True while an engine instance exists (including shutting down until <see cref="StopEngineAsync"/> clears it).</summary>
    private bool IsEngineRunning() => _engine is not null;
}
