using System.Text.Json;

namespace MorpheusEngine.Tests.Integration.Helpers;

internal sealed class TestConfigBuilder
{
    private readonly string _repositoryRoot;
    private readonly Dictionary<string, int> _ports = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<EngineModuleInfo> _modules = [];
    private readonly Dictionary<string, string> _aliases = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, EngineTurnPipelineInfo> _turnPipelines = new(StringComparer.OrdinalIgnoreCase);

    public TestConfigBuilder(string? repositoryRoot = null)
    {
        _repositoryRoot = repositoryRoot ?? Path.Combine(Path.GetTempPath(), "morpheus_test_repo");
    }

    public TestConfigBuilder AddModule(
        string portKey,
        int port,
        string? displayName = null,
        bool requiredByEngine = false,
        int loadOrder = 0,
        string? launchArtifact = null,
        IReadOnlyList<EngineEndpointInfo>? endpoints = null,
        GenericLlmProviderModuleOptions? genericLlmProviderOptions = null,
        QwenModuleOptions? qwenOptions = null,
        MemoryDirectorModuleOptions? memoryDirectorOptions = null,
        EmbeddingsModuleOptions? embeddingsOptions = null)
    {
        return AddModule(
            new EngineModuleInfo(
                portKey,
                displayName ?? portKey,
                requiredByEngine,
                loadOrder,
                new EngineModuleLaunchInfo(launchArtifact ?? $"{portKey}.dll"),
                endpoints ?? [],
                genericLlmProviderOptions,
                qwenOptions,
                memoryDirectorOptions,
                embeddingsOptions),
            port);
    }

    public TestConfigBuilder AddModule(EngineModuleInfo module, int port)
    {
        if (string.IsNullOrWhiteSpace(module.PortKey))
        {
            throw new ArgumentException("module.PortKey must be non-empty.", nameof(module));
        }

        _ports[module.PortKey] = port;
        _modules.RemoveAll(existing => string.Equals(existing.PortKey, module.PortKey, StringComparison.OrdinalIgnoreCase));
        _modules.Add(module);
        return this;
    }

    public TestConfigBuilder AddAlias(string logicalKey, string concreteModuleKey)
    {
        if (string.IsNullOrWhiteSpace(logicalKey) || string.IsNullOrWhiteSpace(concreteModuleKey))
        {
            throw new ArgumentException("Alias keys must be non-empty.");
        }

        _aliases[logicalKey.Trim()] = concreteModuleKey.Trim();
        return this;
    }

    public TestConfigBuilder AddPipeline(
        string id,
        IReadOnlyList<EngineTurnPipelineStepInfo> steps,
        string responseSourceStep,
        string responseType)
    {
        return AddPipeline(new EngineTurnPipelineInfo(id, steps, new EngineTurnPipelineResponseMapping(responseSourceStep, responseType)));
    }

    public TestConfigBuilder AddPipeline(EngineTurnPipelineInfo pipeline)
    {
        if (string.IsNullOrWhiteSpace(pipeline.Id))
        {
            throw new ArgumentException("pipeline.Id must be non-empty.", nameof(pipeline));
        }

        _turnPipelines[pipeline.Id] = pipeline;
        return this;
    }

    public EngineConfiguration Build()
    {
        return new EngineConfiguration(
            _repositoryRoot,
            new EnginePortMap(new Dictionary<string, int>(_ports, StringComparer.OrdinalIgnoreCase)),
            _modules.ToArray(),
            new Dictionary<string, string>(_aliases, StringComparer.OrdinalIgnoreCase),
            new Dictionary<string, EngineTurnPipelineInfo>(_turnPipelines, StringComparer.OrdinalIgnoreCase));
    }

    public static string SerializeJson(object value)
    {
        return JsonSerializer.Serialize(value);
    }
}
