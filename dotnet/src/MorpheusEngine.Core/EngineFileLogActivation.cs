namespace MorpheusEngine;

/// <summary>
/// Binds on-disk engine logs (global id, session clock) to a run folder. Call from each process after the UI establishes runId.
/// </summary>
public static class EngineFileLogActivation
{
    /// <summary>
    /// Primary writer (host App): creates run directory, configures sequence, writes a fresh session clock file.
    /// </summary>
    public static void ActivatePrimary(string repositoryRoot, string gameProjectId, string runId)
    {
        var dir = GameRunLogPaths.GetRunSaveDirectory(repositoryRoot, gameProjectId, runId);
        Directory.CreateDirectory(dir);
        GlobalLogSequence.ConfigureForDirectory(dir);
        LogSessionClock.BeginNewSessionInDirectory(dir);
    }

    /// <summary>
    /// Child modules: same directory and sequence file; load session clock from disk (host must have called <see cref="ActivatePrimary"/> first).
    /// </summary>
    public static void ActivateJoin(string repositoryRoot, string gameProjectId, string runId)
    {
        var dir = GameRunLogPaths.GetRunSaveDirectory(repositoryRoot, gameProjectId, runId);
        GlobalLogSequence.ConfigureForDirectory(dir);
        LogSessionClock.LoadFromDirectory(dir);
    }
}
