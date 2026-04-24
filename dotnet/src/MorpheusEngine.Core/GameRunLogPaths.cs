namespace MorpheusEngine;

/// <summary>
/// Path helpers for per-run data under game_projects/(gameProjectId)/saved/(runId)/ (e.g. session SQLite).
/// The engine does not read or write under docs; that tree is for developers only.
/// </summary>
public static class GameRunLogPaths
{
    /// <summary>Same path rules as session SQLite: one folder per run under saved/.</summary>
    public static string GetRunSaveDirectory(string repositoryRoot, string gameProjectId, string runId)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            throw new ArgumentException("repositoryRoot must be non-empty.", nameof(repositoryRoot));
        }

        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId must be non-empty.", nameof(runId));
        }

        return Path.GetFullPath(
            Path.Combine(
                repositoryRoot.Trim(),
                "game_projects",
                gameProjectId.Trim(),
                "saved",
                runId.Trim()));
    }
}
