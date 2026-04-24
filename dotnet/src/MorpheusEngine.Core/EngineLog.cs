using System.Diagnostics;
using System.Text;
using System.Threading;

namespace MorpheusEngine;

/// <summary>
/// Lightweight console logger: prefixes each line with entry id, elapsed time, and module tag.
/// When <see cref="GlobalLogSequence"/> is configured, ids and (with <see cref="LogSessionClock"/>) elapsed time are shared across processes.
/// Child modules launched with redirected stdout omit local console wrapping so the host can prepend a unified prefix.
/// </summary>
public static class EngineLog
{
    /// <summary>
    /// When set to 1 or true, child processes keep local <see cref="PrefixingTextWriter"/> even if stdout is redirected
    /// (the host then forwards lines with <see cref="WriteForwardedLine"/> instead of <see cref="WriteHostedChildLine"/>).
    /// </summary>
    public const string ForceChildPrefixEnvVar = "MORPHEUS_FORCE_CHILD_PREFIX";

    private static readonly Stopwatch _stopwatch = Stopwatch.StartNew();
    private static long _fallbackSequence = 0;
    private static bool _initialized = false;
    private static string _moduleName = string.Empty;

    private static TextWriter? _rawOut;
    private static TextWriter? _rawErr;

    /// <summary>Initializes line prefixing for the current process. Safe to call at most once.</summary>
    public static void Initialize(string moduleName)
    {
        if (_initialized)
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(moduleName))
        {
            throw new ArgumentException("moduleName must be non-empty.", nameof(moduleName));
        }

        _moduleName = moduleName.Trim();
        _rawOut = Console.Out;
        _rawErr = Console.Error;

        // Hosted engine children: stdout/stderr are redirected; omit local prefix so the host can assign one global id per line.
        var useRawConsoleWhenRedirected = Console.IsOutputRedirected && !IsForceChildPrefix();
        if (!useRawConsoleWhenRedirected)
        {
            Console.SetOut(new PrefixingTextWriter(_rawOut, _moduleName, isError: false));
            Console.SetError(new PrefixingTextWriter(_rawErr, _moduleName, isError: true));
        }

        _initialized = true;
    }

    /// <summary>True when this process should emit unprefixed stdout lines for host forwarding (redirected, no force-env).</summary>
    public static bool UsesRawRedirectedConsole =>
        _initialized && Console.IsOutputRedirected && !IsForceChildPrefix();

    private static bool IsForceChildPrefix() =>
        string.Equals(Environment.GetEnvironmentVariable(ForceChildPrefixEnvVar), "1", StringComparison.OrdinalIgnoreCase)
        || string.Equals(Environment.GetEnvironmentVariable(ForceChildPrefixEnvVar), "true", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Formats a prefix string that matches the console prefixing writer, while incrementing the same sequence source as the console.
    /// Intended for non-console sinks (e.g. file logs) that must match the merged export format when global sequence is active.
    /// </summary>
    public static string FormatLinePrefix(bool isError)
    {
        if (!_initialized || string.IsNullOrWhiteSpace(_moduleName))
        {
            throw new InvalidOperationException("EngineLog is not initialized; call EngineLog.Initialize(moduleName) first.");
        }

        return TakeNextLinePrefix(isError);
    }

    /// <summary>Elapsed time as HH:MM:SS::cc (cc = centiseconds, two digits).</summary>
    private static string FormatElapsedSinceStart(TimeSpan elapsed)
    {
        var totalHours = (int)elapsed.TotalHours;
        var minutes = (int)(elapsed.TotalMinutes % 60);
        var seconds = (int)(elapsed.TotalSeconds % 60);
        var centiseconds = elapsed.Milliseconds / 10;
        return $"{totalHours:D2}:{minutes:D2}:{seconds:D2}::{centiseconds:D2}";
    }

    /// <summary>Builds one prefix line segment including trailing space (before the message body).</summary>
    private static string BuildLinePrefix(bool isError, string sourceTag)
    {
        if (string.IsNullOrWhiteSpace(sourceTag))
        {
            throw new ArgumentException("sourceTag must be non-empty.", nameof(sourceTag));
        }

        ulong seq;
        TimeSpan elapsed;
        if (GlobalLogSequence.IsConfigured)
        {
            seq = GlobalLogSequence.AllocateNext();
            elapsed = LogSessionClock.ElapsedSinceSessionStart;
        }
        else
        {
            var n = Interlocked.Increment(ref _fallbackSequence);
            if (n < 0)
            {
                throw new InvalidOperationException("EngineLog fallback sequence overflow.");
            }

            seq = (ulong)n;
            elapsed = _stopwatch.Elapsed;
        }

        var tag = isError ? $"{sourceTag}:ERR" : sourceTag;
        return $"[{seq} ; {FormatElapsedSinceStart(elapsed)}] [{tag}] ";
    }

    private static string TakeNextLinePrefix(bool isError)
    {
        return BuildLinePrefix(isError, _moduleName);
    }

    /// <summary>
    /// Writes a line without adding this process's prefix.
    /// Used when child processes already prefixed their own output, or for verbatim relay.
    /// </summary>
    public static void WriteForwardedLine(string line)
    {
        if (_rawOut is null)
        {
            Console.Out.WriteLine(line);
            return;
        }

        _rawOut.WriteLine(line);
    }

    /// <summary>
    /// Host only: prepends one unified prefix (global id + session elapsed + logical source) then the child message body.
    /// Requires file log activation: <see cref="GlobalLogSequence.ConfigureForDirectory"/> and a session clock on disk (see <see cref="EngineFileLogActivation"/>).
    /// </summary>
    public static void WriteHostedChildLine(string logicalSource, bool isError, string message)
    {
        if (!_initialized || _rawOut is null)
        {
            throw new InvalidOperationException("EngineLog is not initialized; call EngineLog.Initialize(moduleName) first.");
        }

        if (!GlobalLogSequence.IsConfigured)
        {
            throw new InvalidOperationException(
                "GlobalLogSequence.ConfigureForDirectory(runSaveDir) must be called on the host after run init before WriteHostedChildLine.");
        }

        if (string.IsNullOrWhiteSpace(logicalSource))
        {
            throw new ArgumentException("logicalSource must be non-empty.", nameof(logicalSource));
        }

        _rawOut.WriteLine(BuildLinePrefix(isError, logicalSource.Trim()) + message);
    }

    private sealed class PrefixingTextWriter : TextWriter
    {
        private readonly TextWriter _inner;
        private readonly string _moduleName;
        private readonly bool _isError;
        private bool _atLineStart = true;

        public PrefixingTextWriter(TextWriter inner, string moduleName, bool isError)
        {
            _inner = inner ?? throw new ArgumentNullException(nameof(inner));
            _moduleName = moduleName;
            _isError = isError;
        }

        public override Encoding Encoding => _inner.Encoding;

        public override void Write(char value)
        {
            if (_atLineStart)
            {
                WritePrefix();
                _atLineStart = false;
            }

            _inner.Write(value);
            if (value == '\n')
            {
                _atLineStart = true;
            }
        }

        public override void Write(string? value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return;
            }

            // Prefix once per emitted line, including when a single Write contains multiple newlines.
            var span = value.AsSpan();
            var start = 0;
            for (var i = 0; i < span.Length; i++)
            {
                if (_atLineStart)
                {
                    WritePrefix();
                    _atLineStart = false;
                }

                if (span[i] == '\n')
                {
                    // Write up to and including the newline.
                    _inner.Write(span.Slice(start, i - start + 1).ToString());
                    start = i + 1;
                    _atLineStart = true;
                }
            }

            if (start < span.Length)
            {
                _inner.Write(span.Slice(start).ToString());
            }
        }

        public override void WriteLine(string? value)
        {
            if (_atLineStart)
            {
                WritePrefix();
            }

            _inner.WriteLine(value);
            _atLineStart = true;
        }

        private void WritePrefix()
        {
            _inner.Write(BuildLinePrefix(_isError, _moduleName));
        }
    }
}
