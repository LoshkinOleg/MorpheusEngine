using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

public sealed record GameProjectManifest(
    string Id,
    string Title,
    string TurnPipeline,
    IReadOnlyList<string> RequiredModules);

public static class GameProjectManifestLoader
{
    private sealed class ManifestDto
    {
        public string? Id { get; set; }
        public string? Title { get; set; }

        [JsonPropertyName("required_modules")]
        public List<string>? RequiredModules { get; set; }

        [JsonPropertyName("turn_pipeline")]
        public string? TurnPipeline { get; set; }
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true
    };

    public static GameProjectManifest Load(string repositoryRoot, string gameProjectId)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            throw new ArgumentException("repositoryRoot must be non-empty.", nameof(repositoryRoot));
        }

        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        var trimmedId = gameProjectId.Trim();
        if (trimmedId.Contains("..", StringComparison.Ordinal)
            || trimmedId.Contains('/', StringComparison.Ordinal)
            || trimmedId.Contains('\\', StringComparison.Ordinal))
        {
            throw new ArgumentException("gameProjectId must not contain path separators or '..'.", nameof(gameProjectId));
        }

        var manifestPath = Path.Combine(repositoryRoot, "game_projects", trimmedId, "manifest.json");
        if (!File.Exists(manifestPath))
        {
            throw new FileNotFoundException($"Game project manifest not found at '{manifestPath}'.", manifestPath);
        }

        string json;
        try
        {
            json = File.ReadAllText(manifestPath);
        }
        catch (Exception e)
        {
            throw new InvalidOperationException($"Failed to read game project manifest at '{manifestPath}'.", e);
        }

        ManifestDto dto;
        try
        {
            dto = JsonSerializer.Deserialize<ManifestDto>(json, JsonOptions)
                ?? throw new InvalidOperationException("Manifest JSON deserialized to null.");
        }
        catch (JsonException e)
        {
            throw new InvalidOperationException($"Invalid JSON in game project manifest at '{manifestPath}'.", e);
        }

        var id = (dto.Id ?? string.Empty).Trim();
        if (string.IsNullOrWhiteSpace(id))
        {
            throw new InvalidOperationException($"Manifest at '{manifestPath}' must define a non-empty 'id'.");
        }

        if (!string.Equals(id, trimmedId, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"Manifest id '{id}' does not match game project folder '{trimmedId}' for '{manifestPath}'.");
        }

        var title = (dto.Title ?? string.Empty).Trim();
        if (string.IsNullOrWhiteSpace(title))
        {
            throw new InvalidOperationException($"Manifest at '{manifestPath}' must define a non-empty 'title'.");
        }

        var required = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (dto.RequiredModules is not null)
        {
            foreach (var moduleKey in dto.RequiredModules)
            {
                var key = (moduleKey ?? string.Empty).Trim();
                if (string.IsNullOrWhiteSpace(key))
                {
                    throw new InvalidOperationException($"Manifest at '{manifestPath}' has an empty entry under required_modules.");
                }

                if (key.Contains("..", StringComparison.Ordinal)
                    || key.Contains('/', StringComparison.Ordinal)
                    || key.Contains('\\', StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(
                        $"Manifest required_modules contains invalid module key '{key}' in '{manifestPath}' (no path separators or '..').");
                }

                required.Add(key);
            }
        }

        var turnPipeline = (dto.TurnPipeline ?? "memory_director_default").Trim();
        if (string.IsNullOrWhiteSpace(turnPipeline))
        {
            throw new InvalidOperationException($"Manifest at '{manifestPath}' has an empty turn_pipeline.");
        }

        if (turnPipeline.Contains("..", StringComparison.Ordinal)
            || turnPipeline.Contains('/', StringComparison.Ordinal)
            || turnPipeline.Contains('\\', StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Manifest turn_pipeline contains invalid pipeline id '{turnPipeline}' in '{manifestPath}' (no path separators or '..').");
        }

        return new GameProjectManifest(id, title, turnPipeline, required.ToArray());
    }
}

