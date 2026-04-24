using System.Globalization;
using System.Threading;

namespace MorpheusEngine;

/// <summary>
/// Cross-process monotonic log entry id allocator (mutex + persisted file under the active run save directory).
/// Distinct mutex name from <see cref="OllamaTrafficFile"/> to avoid deadlocks.
/// </summary>
public static class GlobalLogSequence
{
    private const string MutexName = @"Local\MorpheusEngine_LogEntrySeq";

    private static readonly object ConfigureSync = new();
    private static string? _stateDirectory;

    /// <summary>True after <see cref="ConfigureForDirectory"/>; <see cref="AllocateNext"/> requires configuration.</summary>
    public static bool IsConfigured
    {
        get
        {
            lock (ConfigureSync)
            {
                return _stateDirectory is not null;
            }
        }
    }

    /// <summary>
    /// Absolute directory that contains <see cref="GameRunLogPaths.LogEntrySeqFileName"/> (typically the run folder under game_projects/.../saved/runId/).
    /// </summary>
    public static void ConfigureForDirectory(string absoluteStateDirectory)
    {
        if (string.IsNullOrWhiteSpace(absoluteStateDirectory))
        {
            throw new ArgumentException("absoluteStateDirectory must be non-empty.", nameof(absoluteStateDirectory));
        }

        lock (ConfigureSync)
        {
            _stateDirectory = Path.GetFullPath(absoluteStateDirectory.Trim());
        }
    }

    /// <summary>Returns the next allocated id (starts at 1 when the state file is absent).</summary>
    public static ulong AllocateNext()
    {
        string dir;
        lock (ConfigureSync)
        {
            if (_stateDirectory is null)
            {
                throw new InvalidOperationException(
                    "GlobalLogSequence.ConfigureForDirectory must be called before AllocateNext() (file logging activates after run init).");
            }

            dir = _stateDirectory;
        }

        var statePath = Path.Combine(dir, GameRunLogPaths.LogEntrySeqFileName);
        using var mutex = new Mutex(initiallyOwned: false, name: MutexName);
        var lockTaken = false;
        try
        {
            try
            {
                mutex.WaitOne();
                lockTaken = true;
            }
            catch (AbandonedMutexException)
            {
                lockTaken = true;
            }

            if (!string.IsNullOrWhiteSpace(dir))
            {
                Directory.CreateDirectory(dir);
            }

            ulong nextToReturn;
            if (!File.Exists(statePath))
            {
                nextToReturn = 1;
                File.WriteAllText(statePath, "2", System.Text.Encoding.UTF8);
                return nextToReturn;
            }

            var text = File.ReadAllText(statePath).Trim();
            if (!ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var storedNext))
            {
                throw new InvalidOperationException(
                    $"Global log sequence state at '{statePath}' is corrupt (expected non-negative integer, got '{text}').");
            }

            if (storedNext == 0)
            {
                throw new InvalidOperationException($"Global log sequence state at '{statePath}' must be >= 1.");
            }

            nextToReturn = storedNext;
            var writeBack = storedNext + 1;
            if (writeBack < storedNext)
            {
                throw new InvalidOperationException("Global log sequence overflow (ulong).");
            }

            File.WriteAllText(statePath, writeBack.ToString(CultureInfo.InvariantCulture), System.Text.Encoding.UTF8);
            return nextToReturn;
        }
        finally
        {
            if (lockTaken)
            {
                mutex.ReleaseMutex();
            }
        }
    }
}
