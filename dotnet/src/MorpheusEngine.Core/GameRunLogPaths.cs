namespace MorpheusEngine;

/// <summary>
/// Paths for engine-written logs under game_projects/(gameProjectId)/saved/(runId)/.
/// The engine does not read or write under docs; that tree is for developers only.
/// </summary>
public static class GameRunLogPaths
{
    public const string OllamaTrafficFileName = "ollama_traffic.log";
    public const string LogEntrySeqFileName = "logEntrySeq.state";
    public const string LogSessionStartFileName = "logSessionStartUtc.txt";

    /// <summary>Same path rules as session SQLite: one folder per run under saved/.</summary>
    public static string GetRunSaveDirectory(string repositoryRoot, string gameProjectId, string runId)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            throw new ArgumentException("repositoryRoot must be non-empty.", nameof(repositoryRoot));
        }

        RequireSafePathSegment(nameof(gameProjectId), gameProjectId);
        RequireSafePathSegment(nameof(runId), runId);

        return Path.GetFullPath(
            Path.Combine(
                repositoryRoot.Trim(),
                "game_projects",
                gameProjectId.Trim(),
                "saved",
                runId.Trim()));
    }

    public static string GetTrafficLogPath(string repositoryRoot, string gameProjectId, string runId) =>
        Path.Combine(GetRunSaveDirectory(repositoryRoot, gameProjectId, runId), OllamaTrafficFileName);

    public static string GetLogEntrySeqPath(string repositoryRoot, string gameProjectId, string runId) =>
        Path.Combine(GetRunSaveDirectory(repositoryRoot, gameProjectId, runId), LogEntrySeqFileName);

    public static string GetLogSessionStartPath(string repositoryRoot, string gameProjectId, string runId) =>
        Path.Combine(GetRunSaveDirectory(repositoryRoot, gameProjectId, runId), LogSessionStartFileName);

    /// <summary>Prevents path traversal via gameProjectId or runId.</summary>
    public static void RequireSafePathSegment(string name, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new ArgumentException($"{name} must be non-empty.");
        }

        var trimmed = value.Trim();
        if (!string.Equals(trimmed, value, StringComparison.Ordinal))
        {
            throw new ArgumentException($"{name} must not have leading or trailing whitespace.");
        }

        if (value.Contains("..", StringComparison.Ordinal))
        {
            throw new ArgumentException($"{name} must not contain '..'.");
        }

        if (value.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar, '/', '\\']) >= 0)
        {
            throw new ArgumentException($"{name} must not contain path separators.");
        }
    }
}
