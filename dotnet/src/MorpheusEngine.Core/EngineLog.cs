using System.Diagnostics;
using System.Text;
using System.Threading;

namespace MorpheusEngine;

/// <summary>
/// Lightweight console logger for host-owned prefixing and forwarding.
/// Intended usage policy: initialize and use this logger in host processes (Engine and App), not in module implementation processes.
/// Module implementations should write with Console methods and let the host prepend unified prefixes while forwarding child output.
/// </summary>
public static class EngineLog
{
#region Nested types
    /// <summary>
    /// Decorator over <see cref="TextWriter"/> that injects one prefix per emitted line.
    /// Design note: this type both "is a" TextWriter (substitutable for Console.SetOut/SetError) and "has a"
    /// TextWriter (<see cref="_inner"/>) so it can delegate actual output to the original writer after prefixing.
    /// </summary>
    private sealed class PrefixingTextWriter : TextWriter
    {
        private readonly TextWriter _inner;
        private readonly string _sourceName;
        private readonly bool _isError;
        private bool _atLineStart = true;

        public PrefixingTextWriter(TextWriter inner, string sourceName, bool isError)
        {
            _inner = inner ?? throw new ArgumentNullException(nameof(inner));
            _sourceName = sourceName;
            _isError = isError;
        }

        #region Overrides of TextWriter
        public override Encoding Encoding => _inner.Encoding;

        /// <summary>
        /// Handles fragmented output safely by prefixing only at line boundaries.
        /// We intentionally write directly to <see cref="_inner"/> instead of calling base.Write(...):
        /// the base path cannot preserve this wrapper's line-state contract, and prefix+char writes would allocate
        /// and risk over-prefixing when callers emit text char-by-char.
        /// </summary>
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

        /// <summary>
        /// Prefixes each logical line in a string, including multi-line payloads in a single call.
        /// This exists even if most call sites use WriteLine(string): TextWriter callers are not required to emit
        /// complete lines in one call, so we preserve correctness across partial/fragmented writes.
        /// </summary>
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

        /// <summary>
        /// Fast path for line-oriented calls. Prefixes once, writes the line, then resets line-start state.
        /// </summary>
        public override void WriteLine(string? value)
        {
            if (_atLineStart)
            {
                WritePrefix();
            }

            _inner.WriteLine(value);
            _atLineStart = true;
        }
        #endregion

        private void WritePrefix()
        {
            _inner.Write(BuildLinePrefix(_isError, _sourceName));
        }
    }
#endregion

    // List of sources that are allowed to run the EngineLog.
    private static readonly string[] _allowedSources =
    [
        "App"
    ];

    private static readonly Stopwatch _stopwatch = Stopwatch.StartNew(); // Starts as soon as runtime has started due to the static keyword.
    private static long currentLogId = 0;
    private static bool _initialized = false; // Note: a stateful static class. This smells.
    private static string _source = string.Empty; // Source this logger runs on. Can only be "App" in our current setup. Keeping in for extensibility.

    private static TextWriter? _rawOut;
    private static TextWriter? _rawErr;

    /// <summary>
    /// Initializes line prefixing for the current process. Safe to call at most once.
    /// Host-only policy: do not call from module implementation processes.
    /// </summary>
    public static void Initialize(string sourceName)
    {
        if (string.IsNullOrWhiteSpace(sourceName))
        {
            throw new ArgumentException("moduleName must be non-empty.", nameof(sourceName));
        }

        if (_initialized)
        {
            return;
        }

        _source = sourceName.Trim();
        if (!Array.Exists(_allowedSources, name => string.Equals(name, _source, StringComparison.Ordinal)))
        {
            throw new InvalidOperationException(
                $"EngineLog.Initialize is host-only. moduleName '{_source}' is not allowed. Allowed values: {string.Join(", ", _allowedSources)}.");
        }

        _rawOut = Console.Out;
        _rawErr = Console.Error;

        // Hosted engine children: stdout/stderr are redirected; omit local prefix so the host can assign one global id per line.
        // Uncomment if redirected output needs to be handled.
        // if (!Console.IsOutputRedirected)
        // {
        //     Console.SetOut(new PrefixingTextWriter(_rawOut, _source, isError: false));
        //     Console.SetError(new PrefixingTextWriter(_rawErr, _source, isError: true));
        // }

        Console.SetOut(new PrefixingTextWriter(_rawOut, _source, isError: false)); // Anything written via Console.WriteLine() on the host will be redirected to a PrefixingTextWriter.
        Console.SetError(new PrefixingTextWriter(_rawErr, _source, isError: true));

        _initialized = true;
    }

    /// <summary>
    /// Host only: prepends one unified prefix (entry id + elapsed + logical source) then the child message body.
    /// </summary>
    public static void WriteHostedChildLine(string logicalSource, bool isError, string message)
    {
        if (!_initialized || _rawOut is null)
        {
            throw new InvalidOperationException("EngineLog is not initialized; call EngineLog.Initialize(moduleName) first.");
        }

        if (string.IsNullOrWhiteSpace(logicalSource))
        {
            throw new ArgumentException("logicalSource must be non-empty.", nameof(logicalSource));
        }

        _rawOut.WriteLine(BuildLinePrefix(isError, logicalSource.Trim()) + message); // This bypasses the PrefixingTextWriter so that we don't end up double tagging the log with both the logicalSource and the host.
    }

    /// <summary>Builds one prefix line segment including trailing space (before the message body).</summary>
    private static string BuildLinePrefix(bool isError, string source)
    {
        if (string.IsNullOrWhiteSpace(source))
        {
            throw new ArgumentException("source must be non-empty.", nameof(source));
        }

        var n = Interlocked.Increment(ref currentLogId);
        if (n < 0)
        {
            throw new InvalidOperationException("EngineLog sequence overflow.");
        }

        var seq = (ulong)n;
        var elapsed = _stopwatch.Elapsed;
        var totalHours = (int)elapsed.TotalHours;
        var minutes = (int)(elapsed.TotalMinutes % 60);
        var seconds = (int)(elapsed.TotalSeconds % 60);
        var centiseconds = elapsed.Milliseconds / 10;

        var tag = isError ? $"{source}:ERR" : source;
        return $"[{seq} ; {totalHours:D2}:{minutes:D2}:{seconds:D2}::{centiseconds:D2}] [{tag}] ";
    }
}
