using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
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

    private enum EngineStatusState
    {
        Stopped,
        Initializing,
        ShuttingDown,
        Ready,
        Processing,
        Error,
    }

    private static readonly SolidColorBrush EngineStatusBrushStopped = new(Color.FromRgb(90, 95, 106));
    private static readonly SolidColorBrush EngineStatusBrushReady = new(Color.FromRgb(61, 204, 119));
    private static readonly SolidColorBrush EngineStatusBrushTransition = new(Color.FromRgb(230, 195, 82));
    private static readonly SolidColorBrush EngineStatusBrushProcessing = new(Color.FromRgb(80, 150, 240));
    private static readonly SolidColorBrush EngineStatusBrushError = new(Color.FromRgb(220, 80, 80));
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
    private const string ResponseTemplateHeader = "Example response (template):\r\n";

    /// <summary>True after <see cref="MorpheusEngine.InitializationCompletedSource"/> completes (host module init; llm_provider_qwen has primed and reports initialized on /health). Enables game UI; not a separate first-token benchmark.</summary>
    private bool _engineModulesInitializedForGame = false;
    private bool _allowClose;
    private bool _shutdownInProgress;
    /// <summary>True while <see cref="StopEngineAsync"/> is tearing down the engine (yellow LED).</summary>
    private bool _engineStopInProgress;
    /// <summary>True after a lifecycle failure for the current run; cleared on next start and when the engine is fully stopped.</summary>
    private bool _engineLifecycleError;
    private bool _suppressEndpointPresetEvents;
    private bool _applyingEndpointFromPreset;
    private bool _gameRequestInFlight;
    /// <summary>Logical game project folder under game_projects/ (mirrors TS layout). Must match an on-disk project; no silent fallback.</summary>
    private string _gameProjectId = "sandcrawler";
    /// <summary>Per-run id; set when the engine is started (run binding happens at engine start).</summary>
    private string _runId = string.Empty;
    /// <summary>Next turn index to send (1-based; must match MAX(snapshots.turn)+1).</summary>
    private int _nextTurn = 1;
    private string[] _qwenMonitorModuleNames = ["Qwen", "LlmProvider_qwen", "llm_provider_qwen"];

    /// <summary>Full console log lines (including newline) for filter re-render.</summary>
    private readonly List<string> _consoleLogBuffer = [];

    /// <summary>Accumulates partial chunks until a newline completes a logical line.</summary>
    private readonly StringBuilder _consoleCurrentLine = new();

    /// <summary>Include filter tokens (module port_key), case-insensitive; empty with no excludes = show all.</summary>
    private string[] _consoleFilterIncludePortKeys = [];

    /// <summary>Exclude filter tokens (module port_key), case-insensitive; lines matching these are dropped (excludes win over includes).</summary>
    private string[] _consoleFilterExcludePortKeys = [];

    /// <summary>Include ranges on EngineLog seq id (union). Empty with no exclude ranges = no range filter.</summary>
    private (long? Start, long? End)[] _consoleFilterIncludeRanges = [];

    /// <summary>Exclude ranges on EngineLog seq id (union).</summary>
    private (long? Start, long? End)[] _consoleFilterExcludeRanges = [];

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
                .SelectMany(module => new[] { module.DisplayName, module.PortKey })
                .Append("Qwen")
                .Append("LlmProvider_qwen")
                .Where(name => !string.IsNullOrWhiteSpace(name))
                .Distinct(StringComparer.OrdinalIgnoreCase)
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
        // Intentionally no-op: qwen monitor log is append-only for the full app session.
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
            HttpRequestBodyTextBox.Text = info.RequestBodyTemplate ?? string.Empty;
            HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
            ApplyResponseTemplateIfNeeded(info);
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

        _engineLifecycleError = false;

        var engine = new MorpheusEngine(_gameProjectId, _runId);
        _engine = engine;
        _engineModulesInitializedForGame = false;

        _engineRunTask = Task.Run(() =>
        {
            try
            {
                engine.Run();
            }
            catch
            {
                _engineLifecycleError = true;
                throw;
            }
            finally
            {
                // Clear task reference on the completing thread so _engineRunTask is not left non-null until a dispatcher tick.
                _engineRunTask = null;
                _engineModulesInitializedForGame = false;
                Dispatcher.BeginInvoke(UpdateButtonState);
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

                _engineModulesInitializedForGame = true;
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

                _engineLifecycleError = true;
                _engineModulesInitializedForGame = false;
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

        _engineStopInProgress = true;
        Dispatcher.Invoke(UpdateButtonState, DispatcherPriority.Normal);

        try
        {
            _engineModulesInitializedForGame = false;
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
                    _engineLifecycleError = true;
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
                    _engineLifecycleError = true;
                    Console.WriteLine("[App] Engine task completed with error: " + e.Message);
                }
            }
        }
        finally
        {
            _engineStopInProgress = false;
        }

        await Dispatcher.InvokeAsync(() =>
        {
            _engine = null;
            _engineLifecycleError = false;
            _runId = string.Empty;
            _nextTurn = 1;
            RefreshGameTurnHeader();
            UpdateButtonState();
        });
    }

    private EngineStatusState ResolveEngineStatusState()
    {
        var running = IsEngineRunning();
        if (_engineLifecycleError)
        {
            return EngineStatusState.Error;
        }

        if (running && (_engineStopInProgress || _shutdownInProgress))
        {
            return EngineStatusState.ShuttingDown;
        }

        if (running && !_engineModulesInitializedForGame)
        {
            return EngineStatusState.Initializing;
        }

        if (running && _gameRequestInFlight)
        {
            return EngineStatusState.Processing;
        }

        if (running)
        {
            return EngineStatusState.Ready;
        }

        return EngineStatusState.Stopped;
    }

    private void UpdateButtonState()
    {
        var running = IsEngineRunning();
        StartButton.IsEnabled = !running && !_shutdownInProgress && _config is not null;
        StopButton.IsEnabled = running && !_shutdownInProgress;

        if (EngineStatusEllipse is not null)
        {
            var status = ResolveEngineStatusState();
            EngineStatusEllipse.Fill = status switch
            {
                EngineStatusState.Ready => EngineStatusBrushReady,
                EngineStatusState.Initializing or EngineStatusState.ShuttingDown => EngineStatusBrushTransition,
                EngineStatusState.Processing => EngineStatusBrushProcessing,
                EngineStatusState.Error => EngineStatusBrushError,
                _ => EngineStatusBrushStopped,
            };
            EngineStatusEllipse.ToolTip = status switch
            {
                EngineStatusState.Ready => "Engine running",
                EngineStatusState.Initializing => "Engine initializing",
                EngineStatusState.ShuttingDown => "Engine shutting down",
                EngineStatusState.Processing => "Engine processing turn",
                EngineStatusState.Error => "Engine error (see console)",
                _ => "Engine stopped",
            };
        }

        if (GameChatInteractionRoot is not null)
        {
            // Transcript + composer stay inactive until engine InitializationCompleted (modules healthy; Qwen has completed model priming before initialized=true).
            GameChatInteractionRoot.IsEnabled = running && _engineModulesInitializedForGame;
        }

        if (GameSendButton is not null)
        {
            GameSendButton.IsEnabled = running && _engineModulesInitializedForGame && !_shutdownInProgress && !_gameRequestInFlight;
        }

        if (GameInputTextBox is not null)
        {
            GameInputTextBox.IsEnabled = running && _engineModulesInitializedForGame && !_shutdownInProgress && !_gameRequestInFlight;
        }
    }

    private void AppendLineToPane(string text)
    {
        Dispatcher.BeginInvoke(() =>
        {
            // Qwen monitor: unchanged extraction per chunk (may span partial EngineLog lines).
            if (TryExtractQwenLogLine(text, out var qwenLogLine))
            {
                AppendQwenMonitorEntry(qwenLogLine);
            }

            // Main console: buffer at full-line granularity so the [source] tag is intact (EngineLog splits prefix and body).
            _consoleCurrentLine.Append(text);
            while (true)
            {
                var current = _consoleCurrentLine.ToString();
                var nlIndex = current.IndexOf('\n');
                if (nlIndex < 0)
                {
                    break;
                }

                var completedLine = current[..(nlIndex + 1)];
                _consoleCurrentLine.Remove(0, nlIndex + 1);

                _consoleLogBuffer.Add(completedLine);
                if (LinePassesConsoleFilter(completedLine))
                {
                    ConsolePane.AppendText(completedLine);
                    ConsolePane.CaretIndex = ConsolePane.Text.Length;
                    ConsolePane.ScrollToEnd();
                }
            }
        });
    }

    private void ConsoleFilterTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (ConsoleFilterTextBox is null || ConsolePane is null)
        {
            return;
        }

        var raw = ConsoleFilterTextBox.Text ?? string.Empty;
        var includeSeen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var excludeSeen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var includes = new List<string>();
        var excludes = new List<string>();
        var includeRanges = new List<(long? Start, long? End)>();
        var excludeRanges = new List<(long? Start, long? End)>();
        foreach (var part in raw.Split(',', StringSplitOptions.None))
        {
            var t = part.Trim();
            if (t.Length == 0)
            {
                continue;
            }

            if (t == "-")
            {
                continue;
            }

            if (t.Length >= 3 && t[0] == '-' && t[1] == '(' && t[^1] == ')')
            {
                var inner = t[2..^1];
                if (TryParseRangeBody(inner, out var rs, out var re))
                {
                    excludeRanges.Add((rs, re));
                }

                continue;
            }

            if (t.Length >= 2 && t[0] == '(' && t[^1] == ')')
            {
                var inner = t[1..^1];
                if (TryParseRangeBody(inner, out var rs, out var re))
                {
                    includeRanges.Add((rs, re));
                }

                continue;
            }

            if (t[0] == '-')
            {
                var key = t[1..].Trim();
                if (key.Length == 0 || !excludeSeen.Add(key))
                {
                    continue;
                }

                excludes.Add(key);
            }
            else
            {
                if (!includeSeen.Add(t))
                {
                    continue;
                }

                includes.Add(t);
            }
        }

        _consoleFilterIncludePortKeys = includes.ToArray();
        _consoleFilterExcludePortKeys = excludes.ToArray();
        _consoleFilterIncludeRanges = includeRanges.ToArray();
        _consoleFilterExcludeRanges = excludeRanges.ToArray();

        ConsolePane.Clear();
        foreach (var line in _consoleLogBuffer)
        {
            if (LinePassesConsoleFilter(line))
            {
                ConsolePane.AppendText(line);
            }
        }

        ConsolePane.CaretIndex = ConsolePane.Text.Length;
        ConsolePane.ScrollToEnd();
    }

    /// <summary>Parses EngineLog line shape: <c>[seq ; time] [source] body</c>. Returns false if the second bracket pair is missing.</summary>
    private static bool TryParseConsoleLogSourceTag(string line, out string sourceTag)
    {
        sourceTag = string.Empty;
        var trimmed = line.TrimStart('\r', '\n');
        if (trimmed.Length < 5 || trimmed[0] != '[')
        {
            return false;
        }

        var firstClose = trimmed.IndexOf(']');
        if (firstClose < 0)
        {
            return false;
        }

        var secondOpen = trimmed.IndexOf('[', firstClose + 1);
        if (secondOpen < 0)
        {
            return false;
        }

        var secondClose = trimmed.IndexOf(']', secondOpen + 1);
        if (secondClose <= secondOpen)
        {
            return false;
        }

        sourceTag = trimmed.Substring(secondOpen + 1, secondClose - secondOpen - 1);
        if (sourceTag.EndsWith(":ERR", StringComparison.OrdinalIgnoreCase))
        {
            sourceTag = sourceTag[..^4];
        }

        return true;
    }

    /// <summary>Parses the seq id from the first bracket group of an EngineLog line: <c>[seq ; time]</c>.</summary>
    private static bool TryParseConsoleLogSeq(string line, out long seq)
    {
        seq = 0;
        var trimmed = line.TrimStart('\r', '\n');
        if (trimmed.Length < 5 || trimmed[0] != '[')
        {
            return false;
        }

        var firstClose = trimmed.IndexOf(']');
        if (firstClose < 0)
        {
            return false;
        }

        var inner = trimmed.Substring(1, firstClose - 1);
        var sep = inner.IndexOf(" ;", StringComparison.Ordinal);
        if (sep < 0)
        {
            return false;
        }

        var seqPart = inner[..sep].Trim();
        if (seqPart.Length == 0 || !long.TryParse(seqPart, NumberStyles.Integer, CultureInfo.InvariantCulture, out seq) || seq < 0)
        {
            return false;
        }

        return true;
    }

    /// <summary>Parses <c>start;end</c> with empty sides as open bounds. Rejects literal <c>-</c> as a bound.</summary>
    private static bool TryParseRangeBody(string body, out long? start, out long? end)
    {
        start = null;
        end = null;
        var semi = body.IndexOf(';');
        if (semi < 0)
        {
            return false;
        }

        var left = body[..semi].Trim();
        var right = body[(semi + 1)..].Trim();
        if (right.Contains(';'))
        {
            return false;
        }

        if (left == "-")
        {
            return false;
        }

        if (right == "-")
        {
            return false;
        }

        if (left.Length > 0)
        {
            if (!long.TryParse(left, NumberStyles.Integer, CultureInfo.InvariantCulture, out var s) || s < 0)
            {
                return false;
            }

            start = s;
        }

        if (right.Length > 0)
        {
            if (!long.TryParse(right, NumberStyles.Integer, CultureInfo.InvariantCulture, out var e) || e < 0)
            {
                return false;
            }

            end = e;
        }

        if (start is { } a && end is { } b && a > b)
        {
            return false;
        }

        return true;
    }

    private static bool SeqInRange(long seq, (long? Start, long? End) r) =>
        (r.Start is null || seq >= r.Start) && (r.End is null || seq <= r.End);

    private bool LinePassesRangeFilter(string line)
    {
        var hasIncl = _consoleFilterIncludeRanges.Length > 0;
        var hasExcl = _consoleFilterExcludeRanges.Length > 0;
        if (!hasIncl && !hasExcl)
        {
            return true;
        }

        var hasSeq = TryParseConsoleLogSeq(line, out var seq);
        if (!hasSeq)
        {
            if (hasIncl)
            {
                return false;
            }

            return true;
        }

        if (hasIncl)
        {
            var inAny = false;
            foreach (var r in _consoleFilterIncludeRanges)
            {
                if (SeqInRange(seq, r))
                {
                    inAny = true;
                    break;
                }
            }

            if (!inAny)
            {
                return false;
            }
        }

        foreach (var r in _consoleFilterExcludeRanges)
        {
            if (SeqInRange(seq, r))
            {
                return false;
            }
        }

        return true;
    }

    private bool LinePassesConsoleFilter(string line)
    {
        if (!LinePassesRangeFilter(line))
        {
            return false;
        }

        var hasIncludes = _consoleFilterIncludePortKeys.Length > 0;
        var hasExcludes = _consoleFilterExcludePortKeys.Length > 0;
        if (!hasIncludes && !hasExcludes)
        {
            return true;
        }

        var hasTag = TryParseConsoleLogSourceTag(line, out var tag);

        // Excludes win when the line has a parseable tag.
        if (hasExcludes && hasTag)
        {
            foreach (var key in _consoleFilterExcludePortKeys)
            {
                if (string.Equals(tag, key, StringComparison.OrdinalIgnoreCase))
                {
                    return false;
                }
            }
        }

        if (!hasIncludes)
        {
            // Pure-exclude mode: untagged lines are shown (cannot match an exclude).
            return true;
        }

        if (!hasTag)
        {
            return false;
        }

        foreach (var key in _consoleFilterIncludePortKeys)
        {
            if (string.Equals(tag, key, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
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
                new TurnRequest(_nextTurn, playerInput.Trim()),
                JsonOptions);
            var result = await SendRequestAsync(_config.GetRequiredListenPort("router"), "/turn", body, "POST");

            if (result.StatusCode is >= 200 and < 300)
            {
                if (TryParseTurnResponse(result.Body, out var turnResponse))
                {
                    AppendEngineGameMessage(FormatTurnResponse(turnResponse), _nextTurn);
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

    private static bool TryParseTurnResponse(string body, out TurnResponse turnResponse)
    {
        turnResponse = null!;

        try
        {
            var parsed = JsonSerializer.Deserialize<TurnResponse>(body, JsonOptions);
            if (parsed is null)
            {
                return false;
            }

            turnResponse = parsed;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static string FormatTurnResponse(TurnResponse response)
    {
        return response.Text;
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
            if (text.StartsWith(normalPrefix, StringComparison.OrdinalIgnoreCase))
            {
                qwenLogLine = text;
                return true;
            }

            var errorPrefix = $"[{moduleName}:ERR] OLLAMA_IO ";
            if (text.StartsWith(errorPrefix, StringComparison.OrdinalIgnoreCase))
            {
                qwenLogLine = text;
                return true;
            }

            // New EngineLog prefix: [entryId ; HH:MM:SS::cc] [LlmProvider_qwen] OLLAMA_IO ...
            var newNormalMarker = $"] [{moduleName}] OLLAMA_IO ";
            var newNormalIdx = text.IndexOf(newNormalMarker, StringComparison.OrdinalIgnoreCase);
            if (newNormalIdx >= 0)
            {
                qwenLogLine = text;
                return true;
            }

            var newErrMarker = $"] [{moduleName}:ERR] OLLAMA_IO ";
            var newErrIdx = text.IndexOf(newErrMarker, StringComparison.OrdinalIgnoreCase);
            if (newErrIdx >= 0)
            {
                qwenLogLine = text;
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
                        if (string.Equals(tag, moduleName, StringComparison.OrdinalIgnoreCase)
                            || string.Equals(tag, moduleName + ":ERR", StringComparison.OrdinalIgnoreCase))
                        {
                            var afterTag = tagEnd + 1;
                            if (afterTag < text.Length && text[afterTag] == ' ')
                            {
                                afterTag++;
                            }

                            const string ollamaPrefix = "OLLAMA_IO ";
                            if (afterTag + ollamaPrefix.Length <= text.Length
                                && string.Equals(text.Substring(afterTag, ollamaPrefix.Length), ollamaPrefix, StringComparison.OrdinalIgnoreCase))
                            {
                                qwenLogLine = text;
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
        if (match?.RequestBodyTemplate is null)
        {
            return;
        }

        HttpRequestBodyTextBox.Text = match.RequestBodyTemplate;
        HttpRequestBodyTextBox.CaretIndex = HttpRequestBodyTextBox.Text.Length;
        ApplyResponseTemplateIfNeeded(match);
    }

    private void ApplyResponseTemplateIfNeeded(EngineEndpointInfo endpoint)
    {
        if (HttpResponsePane is null)
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(endpoint.ResponseBodyTemplate))
        {
            return;
        }

        var current = HttpResponsePane.Text ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(current) && !current.StartsWith(ResponseTemplateHeader, StringComparison.Ordinal))
        {
            return; // Preserve the last real response unless the pane is empty or already showing a template.
        }

        HttpResponsePane.Text = ResponseTemplateHeader + endpoint.ResponseBodyTemplate;
    }

    /// <summary>True while an engine instance exists (including shutting down until <see cref="StopEngineAsync"/> clears it).</summary>
    private bool IsEngineRunning() => _engine is not null;
}
