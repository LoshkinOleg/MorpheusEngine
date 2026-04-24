namespace MorpheusEngine;

/// <summary>
/// UTC session anchor file under the active run directory so all processes share the same "elapsed since log session" time in prefixes.
/// </summary>
public static class LogSessionClock
{
    private static readonly object Sync = new();
    private static DateTime _sessionStartUtc;
    private static bool _loaded;

    /// <summary>Host or any module: overwrite session start in the run directory (call once per activation).</summary>
    public static void BeginNewSessionInDirectory(string absoluteRunSaveDirectory)
    {
        if (string.IsNullOrWhiteSpace(absoluteRunSaveDirectory))
        {
            throw new ArgumentException("absoluteRunSaveDirectory must be non-empty.", nameof(absoluteRunSaveDirectory));
        }

        var dir = Path.GetFullPath(absoluteRunSaveDirectory.Trim());
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, GameRunLogPaths.LogSessionStartFileName);

        var utc = DateTime.UtcNow;
        File.WriteAllText(path, utc.Ticks.ToString(), System.Text.Encoding.UTF8);
        lock (Sync)
        {
            _sessionStartUtc = utc;
            _loaded = true;
        }
    }

    /// <summary>Load session anchor from disk (e.g. after activate in a child process).</summary>
    public static void LoadFromDirectory(string absoluteRunSaveDirectory)
    {
        if (string.IsNullOrWhiteSpace(absoluteRunSaveDirectory))
        {
            throw new ArgumentException("absoluteRunSaveDirectory must be non-empty.", nameof(absoluteRunSaveDirectory));
        }

        lock (Sync)
        {
            if (_loaded)
            {
                return;
            }

            var dir = Path.GetFullPath(absoluteRunSaveDirectory.Trim());
            var path = Path.Combine(dir, GameRunLogPaths.LogSessionStartFileName);
            if (!File.Exists(path))
            {
                throw new InvalidOperationException(
                    $"Log session file missing at '{path}'; activate file logging after run init before relying on LogSessionClock.");
            }

            var text = File.ReadAllText(path).Trim();
            if (!long.TryParse(text, out var ticks))
            {
                throw new InvalidOperationException(
                    $"Log session file at '{path}' is corrupt (expected Int64 ticks, got '{text}').");
            }

            _sessionStartUtc = new DateTime(ticks, DateTimeKind.Utc);
            _loaded = true;
        }
    }

    /// <summary>Elapsed since session anchor; call only after <see cref="BeginNewSessionInDirectory"/> or <see cref="LoadFromDirectory"/>.</summary>
    public static TimeSpan ElapsedSinceSessionStart
    {
        get
        {
            lock (Sync)
            {
                if (!_loaded)
                {
                    throw new InvalidOperationException(
                        "Log session not initialized; call EngineFileLogActivation or LogSessionClock.LoadFromDirectory first.");
                }

                return DateTime.UtcNow - _sessionStartUtc;
            }
        }
    }
}
