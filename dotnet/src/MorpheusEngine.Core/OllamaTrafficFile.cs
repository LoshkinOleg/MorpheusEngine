using System.Text;
using System.Threading;

namespace MorpheusEngine;

/// <summary>
/// Append-only log for full engine ↔ bundled Ollama traffic under the run save directory (never under docs).
/// Writes are serialized with a named mutex so the router process and LLM provider process do not corrupt each other's appends.
/// </summary>
public sealed class OllamaTrafficFile
{
    private const string CrossProcessMutexName = @"Local\MorpheusEngine_LlmTrafficLog";

    private readonly string _trafficLogFilePath;

    /// <param name="trafficLogFilePath">Full path to <see cref="GameRunLogPaths.OllamaTrafficFileName"/> (or equivalent).</param>
    public OllamaTrafficFile(string trafficLogFilePath)
    {
        if (string.IsNullOrWhiteSpace(trafficLogFilePath))
        {
            throw new ArgumentException("trafficLogFilePath must be non-empty.", nameof(trafficLogFilePath));
        }

        _trafficLogFilePath = Path.GetFullPath(trafficLogFilePath.Trim());
    }

    /// <summary>Appends one UTF-8 line (creates parent directory if needed).</summary>
    public static void AppendLine(string trafficLogFilePath, string line)
    {
        if (line is null)
        {
            throw new ArgumentNullException(nameof(line));
        }

        if (string.IsNullOrWhiteSpace(trafficLogFilePath))
        {
            throw new ArgumentException("trafficLogFilePath must be non-empty.", nameof(trafficLogFilePath));
        }

        var path = Path.GetFullPath(trafficLogFilePath.Trim());
        using var mutex = new Mutex(initiallyOwned: false, name: CrossProcessMutexName);
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

            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.AppendAllText(path, line + Environment.NewLine, Encoding.UTF8);
        }
        finally
        {
            if (lockTaken)
            {
                mutex.ReleaseMutex();
            }
        }
    }

    public void AppendFullLine(string line)
    {
        AppendLine(_trafficLogFilePath, line);
    }
}
