using System.Text.Json;

namespace MorpheusEngine;

/// <summary>Shared POST body handling for POST /engine_log/activate (same JSON as bind_run).</summary>
public static class EngineLogActivateCommands
{
    public readonly record struct ActivateResult(bool Ok, string? ErrorMessage, string? GameProjectId, string? RunId);

    /// <summary>Parses <see cref="InitializeModuleRequest"/> JSON and runs <see cref="EngineFileLogActivation"/>.</summary>
    public static ActivateResult TryActivateFromJsonBody(
        string body,
        string repositoryRoot,
        bool primaryNotJoin,
        JsonSerializerOptions jsonOptions)
    {
        InitializeModuleRequest? parsed;
        try
        {
            parsed = JsonSerializer.Deserialize<InitializeModuleRequest>(body, jsonOptions);
        }
        catch (JsonException e)
        {
            return new ActivateResult(false, "Invalid JSON payload: " + e.Message, null, null);
        }

        if (parsed is null
            || string.IsNullOrWhiteSpace(parsed.GameProjectId)
            || string.IsNullOrWhiteSpace(parsed.RunId))
        {
            return new ActivateResult(false, "Request must include non-empty gameProjectId and runId.", null, null);
        }

        var gameProjectId = parsed.GameProjectId.Trim();
        var runId = parsed.RunId.Trim();

        try
        {
            if (primaryNotJoin)
            {
                EngineFileLogActivation.ActivatePrimary(repositoryRoot, gameProjectId, runId);
            }
            else
            {
                EngineFileLogActivation.ActivateJoin(repositoryRoot, gameProjectId, runId);
            }
        }
        catch (Exception e)
        {
            return new ActivateResult(false, e.Message, null, null);
        }

        return new ActivateResult(true, null, gameProjectId, runId);
    }
}
