using System.Text.Json;
using System.Text.Json.Nodes;
using MorpheusEngine;

namespace MorpheusEngine.Tests.Integration.Helpers;

internal static class IntegrationEngineConfigurationFixture
{
    /// <summary>Indented JSON serialization for engine_config writes under temp repos.</summary>
    private static readonly JsonSerializerOptions INDENT_JSON = new()
    {
        WriteIndented = true,
    };

    /// <summary>
    /// Loads a committed JSON fixture from the test output Fixtures/Configurations directory.
    /// </summary>
    public static JsonNode LoadConfigurationsFixture(string fixtureFileName)
    {
        if (string.IsNullOrWhiteSpace(fixtureFileName))
        {
            throw new ArgumentException("fixtureFileName must be non-empty.", nameof(fixtureFileName));
        }

        var trimmed = fixtureFileName.Trim();
        var path = Path.Combine(AppContext.BaseDirectory, "Fixtures", "Configurations", trimmed);
        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                $"Integration engine-config fixture '{trimmed}' was not copied next to the test assembly (expected '{path}').");
        }

        var text = File.ReadAllText(path);
        return JsonNode.Parse(text)
            ?? throw new InvalidOperationException($"Fixture '{trimmed}' did not deserialize to JSON content.");
    }

    /// <summary>
    /// Applies memory-director-specific numeric options on the fixture row keyed by modules[].port_key == memory_director.
    /// </summary>
    public static void PatchMemoryDirectorOptions(
        JsonNode root,
        int maxStepsPerTurn,
        int maxToolResultChars,
        int maxFullMessages,
        string keepModelLoadedFor = "30m")
    {
        if (root is not JsonObject)
        {
            throw new ArgumentException("root must deserialize to a JSON object.", nameof(root));
        }

        if (root["modules"] is not JsonArray modules)
        {
            throw new InvalidOperationException("Fixture root must declare a non-null modules array.");
        }

        var memoryDirector = modules
            .OfType<JsonObject>()
            .FirstOrDefault(candidate =>
                string.Equals(
                    candidate["port_key"]?.GetValue<string>().Trim(),
                    "memory_director",
                    StringComparison.OrdinalIgnoreCase));

        if (memoryDirector is null)
        {
            throw new InvalidOperationException(
                "Fixture modules[] does not declare a memory_director port_key row to patch behavioral options.");
        }

        memoryDirector["max_steps_per_turn"] = maxStepsPerTurn;
        memoryDirector["max_tool_result_chars"] = maxToolResultChars;
        memoryDirector["max_full_messages"] = maxFullMessages;
        memoryDirector["keep_model_loaded_for"] = keepModelLoadedFor;
    }

    /// <summary>Writes indented engine_config.json at the repository root (temp game repo).</summary>
    public static void WriteEngineConfigJson(string repositoryRoot, JsonNode configurationDocument)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            throw new ArgumentException("repositoryRoot must be non-empty.", nameof(repositoryRoot));
        }

        if (configurationDocument is null)
        {
            throw new ArgumentNullException(nameof(configurationDocument));
        }

        var repo = repositoryRoot.Trim();
        var destination = Path.Combine(repo, "engine_config.json");
        Directory.CreateDirectory(repo);
        File.WriteAllText(destination, configurationDocument.ToJsonString(INDENT_JSON));
    }

    /// <summary>
    /// Reloads cached configuration exactly like production: writes must already be flushed to disk.
    /// Ordering: EngineConfigLoader.ResetForTesting clears both the cache and repo-root override callers must restore it after Reset.
    /// </summary>
    public static EngineConfiguration LoadConfigurationViaEngineConfigLoader(string repositoryRoot)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            throw new ArgumentException("repositoryRoot must be non-empty.", nameof(repositoryRoot));
        }

        var repo = repositoryRoot.Trim();
        var path = Path.Combine(repo, "engine_config.json");
        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                $"engine_config.json must exist before loading via {nameof(EngineConfigLoader)} (expected '{path}').");
        }

        EngineConfigLoader.ResetForTesting();
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(repo);
        return EngineConfigLoader.GetConfiguration();
    }
}
