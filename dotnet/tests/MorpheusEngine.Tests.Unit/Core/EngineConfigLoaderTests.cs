using System.Text.Json;
using System.Text.Json.Nodes;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Fixtures;
using MorpheusEngine.Tests.Unit.Helpers;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
[Collection("EngineProcessState")]
public sealed class EngineConfigLoaderTests : IDisposable
{
    private readonly string _originalCurrentDirectory = Environment.CurrentDirectory;

    public EngineConfigLoaderTests()
    {
        EngineConfigLoader.ResetForTesting();
        EngineLog.ResetForTesting();
    }

    [Fact]
    // Verifies that a valid engine config loads with the expected modules, aliases, and pipelines.
    public void EngineConfigLoader_ValidConfig_LoadsSuccessfully()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());

        configuration.ModulesInfos.Should().HaveCount(5);
        configuration.PortMap.Should().NotBeNull();
        configuration.ModuleAliases.Should().ContainKey("generic_llm_provider");
        configuration.TurnPipelines.Should().ContainKey("memory_director_default");
        configuration.ResolveProxyTargetModuleKey("generic_director").Should().Be("memory_director");
        configuration.GetRequiredListenPort("router").Should().Be(19100);
        configuration.FindModule("router").Should().NotBeNull();
        configuration.GetRequiredTurnPipeline("memory_director_default").Steps.Should().HaveCount(2);
    }

    [Fact]
    // Verifies that loading without an engine config file throws a configuration error.
    public void EngineConfigLoader_MissingEngineConfig_ThrowsEngineConfigurationException()
    {
        using var tempRepository = new TempRepository(writeEngineConfig: false);
        ConfigureProcessForRepositoryRoot(tempRepository.RepositoryRoot);

        var act = () => EngineConfigLoader.GetConfiguration();

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*engine_config.json not found*");
    }

    [Fact]
    // Verifies that malformed engine config JSON is wrapped in a configuration exception.
    public void EngineConfigLoader_MalformedJson_ThrowsEngineConfigurationExceptionWrappingJsonException()
    {
        var act = () => LoadConfiguration("{ not valid json }");

        var exception = act.Should().Throw<EngineConfigurationException>().Which;
        exception.InnerException.Should().BeOfType<JsonException>();
    }

    [Fact]
    // Verifies that an empty modules array is rejected.
    public void EngineConfigLoader_EmptyModulesArray_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["modules"] = new JsonArray();

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*non-empty 'modules' array*");
    }

    [Fact]
    // Verifies that missing module aliases are rejected.
    public void EngineConfigLoader_MissingModuleAliases_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config.Remove("module_aliases");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*top-level 'module_aliases' object*");
    }

    [Fact]
    // Verifies that the generic LLM provider alias is required.
    public void EngineConfigLoader_MissingGenericLlmProviderAlias_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["module_aliases"]!.AsObject().Remove("generic_llm_provider");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must map 'generic_llm_provider'*");
    }

    [Fact]
    // Verifies that the generic director alias is required.
    public void EngineConfigLoader_MissingGenericDirectorAlias_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["module_aliases"]!.AsObject().Remove("generic_director");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must map 'generic_director'*");
    }

    [Fact]
    // Verifies that the generic embeddings alias is required.
    public void EngineConfigLoader_MissingGenericEmbeddingsAlias_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["module_aliases"]!.AsObject().Remove("generic_embeddings");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must map 'generic_embeddings'*");
    }

    [Fact]
    // Verifies that duplicate port keys across modules are rejected.
    public void EngineConfigLoader_DuplicatePortKeyAcrossModules_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        modules[4]!["port_key"] = "router";

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*Duplicate port_key*");
    }

    [Fact]
    // Verifies that duplicate listen ports across modules are rejected.
    public void EngineConfigLoader_DuplicatePortAcrossModules_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        modules[1]!["port"] = 19100;

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*Duplicate listen port 19100*");
    }

    [Fact]
    // Verifies that a module missing its port key is rejected.
    public void EngineConfigLoader_ModuleWithoutPortKey_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        modules[0]!.AsObject().Remove("port_key");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*missing port_key*");
    }

    [Fact]
    // Verifies that a module missing its port is rejected.
    public void EngineConfigLoader_ModuleWithoutPort_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        modules[0]!.AsObject().Remove("port");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*missing port*");
    }

    [Fact]
    // Verifies that a module missing its launch command is rejected.
    public void EngineConfigLoader_ModuleWithoutLaunch_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        modules[0]!.AsObject().Remove("launch");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*missing launch*");
    }

    [Fact]
    // Verifies that normalizing an empty path returns the root slash.
    public void EngineConfiguration_NormalizePath_EmptyString_ReturnsSlash()
    {
        EngineConfiguration.NormalizePath(string.Empty).Should().Be("/");
    }

    [Fact]
    // Verifies that normalizing a path adds a leading slash when missing.
    public void EngineConfiguration_NormalizePath_MissingLeadingSlash_ReturnsNormalizedPath()
    {
        EngineConfiguration.NormalizePath("health").Should().Be("/health");
    }

    [Fact]
    // Verifies that normalizing an already rooted path leaves it unchanged.
    public void EngineConfiguration_NormalizePath_ExistingLeadingSlash_PassesThrough()
    {
        EngineConfiguration.NormalizePath("/health").Should().Be("/health");
    }

    [Fact]
    // Verifies that resolving an alias returns its concrete module key.
    public void EngineConfiguration_ResolveProxyTargetModuleKey_Alias_ReturnsConcreteKey()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());

        configuration.ResolveProxyTargetModuleKey("generic_llm_provider").Should().Be("llm_provider_qwen");
    }

    [Fact]
    // Verifies that resolving a non-alias module key returns it unchanged.
    public void EngineConfiguration_ResolveProxyTargetModuleKey_NonAlias_ReturnsInput()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());

        configuration.ResolveProxyTargetModuleKey("router").Should().Be("router");
    }

    [Fact]
    // Verifies that requesting an unknown listen port throws a configuration error.
    public void EngineConfiguration_GetRequiredListenPort_UnknownKey_ThrowsEngineConfigurationException()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());
        var act = () => configuration.GetRequiredListenPort("missing_module");

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*Unknown port_key 'missing_module'*");
    }

    [Fact]
    // Verifies that looking up an unknown module returns null.
    public void EngineConfiguration_FindModule_UnknownKey_ReturnsNull()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());

        configuration.FindModule("missing_module").Should().BeNull();
    }

    [Fact]
    // Verifies that requesting an unknown turn pipeline throws a configuration error.
    public void EngineConfiguration_GetRequiredTurnPipeline_UnknownId_ThrowsEngineConfigurationException()
    {
        var configuration = LoadConfiguration(TestPayloads.BuildMinimalEngineConfigJson());
        var act = () => configuration.GetRequiredTurnPipeline("missing_pipeline");

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*Unknown turn_pipeline 'missing_pipeline'*");
    }

    [Fact]
    // Verifies that a turn pipeline step cannot target an unknown module or alias.
    public void EngineConfigLoader_TurnPipelineReferencingUnknownModule_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["turn_pipelines"]!["memory_director_default"]!["steps"]![0]!["target_module"] = "missing_module";

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*targets unknown module or alias 'missing_module'*");
    }

    [Fact]
    // Verifies that response mapping source steps must reference an existing pipeline step.
    public void EngineConfigLoader_TurnPipelineResponseMappingReferencingUnknownStep_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        config["turn_pipelines"]!["memory_director_default"]!["response_mapping"]!["source_step"] = "missing_step";

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*response_mapping.source_step*must reference an existing step id*");
    }

    [Fact]
    // Verifies that repository root discovery finds the repo by locating engine_config.json.
    public void EngineConfigLoader_FindRepositoryRoot_FindsCurrentRepoRootByEngineConfigPresence()
    {
        var repositoryRoot = EngineConfigLoader.FindRepositoryRoot();

        repositoryRoot.Should().NotBeNull();
        File.Exists(Path.Combine(repositoryRoot!, "engine_config.json")).Should().BeTrue();
        repositoryRoot.Should().Be(Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "..")));
    }

    [Fact]
    // Verifies that filter_off_noisy_logs is only valid on llm_provider_qwen.
    public void EngineConfigLoader_FilterOffNoisyLogsOnRouter_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var routerModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "router", StringComparison.OrdinalIgnoreCase))!;
        routerModule["filter_off_noisy_logs"] = true;

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must not set filter_off_noisy_logs*");
    }

    [Fact]
    // Verifies that filter_off_noisy_logs loads into QwenModuleOptions on llm_provider_qwen.
    public void EngineConfigLoader_FilterOffNoisyLogsOnQwen_LoadsIntoQwenOptions()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var qwenModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "llm_provider_qwen", StringComparison.OrdinalIgnoreCase))!;
        qwenModule["filter_off_noisy_logs"] = true;

        var configuration = LoadConfiguration(ToJson(config));
        var qwen = configuration.FindModule("llm_provider_qwen");

        qwen.Should().NotBeNull();
        qwen!.QwenOptions!.FilterOffNoisyLogs.Should().BeTrue();
    }

    [Fact]
    // Verifies that thinking is only valid on llm_provider_qwen.
    public void EngineConfigLoader_ThinkingOnRouter_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var routerModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "router", StringComparison.OrdinalIgnoreCase))!;
        routerModule["thinking"] = true;

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must not set thinking*");
    }

    [Fact]
    // Verifies that thinking loads into QwenModuleOptions on llm_provider_qwen.
    public void EngineConfigLoader_ThinkingOnQwen_LoadsIntoQwenOptions()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var qwenModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "llm_provider_qwen", StringComparison.OrdinalIgnoreCase))!;
        qwenModule["thinking"] = false;

        var configuration = LoadConfiguration(ToJson(config));
        var qwen = configuration.FindModule("llm_provider_qwen");

        qwen.Should().NotBeNull();
        qwen!.QwenOptions!.Thinking.Should().BeFalse();
    }

    [Fact]
    // Verifies that the active generic LLM provider must define Qwen-specific options.
    public void EngineConfigLoader_ActiveGenericLlmProviderWithoutQwenOptions_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var qwenModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "llm_provider_qwen", StringComparison.OrdinalIgnoreCase))!;
        qwenModule.AsObject().Remove("ollama_port");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must set ollama_port*");
    }

    [Fact]
    // Verifies that the active generic LLM provider must define a default chat model.
    public void EngineConfigLoader_ActiveGenericLlmProviderWithoutDefaultChatModel_ThrowsEngineConfigurationException()
    {
        var config = CreateMinimalConfigNode();
        var modules = config["modules"]!.AsArray();
        var qwenModule = modules.Single(module => string.Equals(module!["port_key"]!.GetValue<string>(), "llm_provider_qwen", StringComparison.OrdinalIgnoreCase))!;
        qwenModule.AsObject().Remove("default_chat_model");

        var act = () => LoadConfiguration(ToJson(config));

        act.Should().Throw<EngineConfigurationException>()
            .WithMessage("*must set default_chat_model*");
    }

    public void Dispose()
    {
        Environment.CurrentDirectory = _originalCurrentDirectory;
        EngineConfigLoader.ResetForTesting();
        EngineLog.ResetForTesting();
    }

    private EngineConfiguration LoadConfiguration(string engineConfigJson)
    {
        using var tempConfig = new TempEngineConfig(engineConfigJson);
        ConfigureProcessForRepositoryRoot(tempConfig.RepositoryRoot);
        var configuration = EngineConfigLoader.GetConfiguration();
        configuration.RepositoryRoot.Should().Be(tempConfig.RepositoryRoot);
        return configuration;
    }

    private void ConfigureProcessForRepositoryRoot(string repositoryRoot)
    {
        Environment.CurrentDirectory = repositoryRoot;
        EngineConfigLoader.SetRepositoryRootOverrideForTesting(repositoryRoot);
    }

    private static JsonObject CreateMinimalConfigNode()
    {
        return JsonNode.Parse(TestPayloads.BuildMinimalEngineConfigJson())!.AsObject();
    }

    private static string ToJson(JsonObject config)
    {
        return config.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true
        });
    }

    private sealed class TempRepository : IDisposable
    {
        public string RepositoryRoot { get; }

        public TempRepository(bool writeEngineConfig)
        {
            RepositoryRoot = Path.Combine(Path.GetTempPath(), "morpheus_repo_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(RepositoryRoot);
            Directory.CreateDirectory(Path.Combine(RepositoryRoot, "dotnet"));
            File.WriteAllText(Path.Combine(RepositoryRoot, "dotnet", "MorpheusEngine.sln"), string.Empty);

            if (writeEngineConfig)
            {
                File.WriteAllText(Path.Combine(RepositoryRoot, "engine_config.json"), TestPayloads.BuildMinimalEngineConfigJson());
            }
        }

        public void Dispose()
        {
            try
            {
                if (Directory.Exists(RepositoryRoot))
                {
                    Directory.Delete(RepositoryRoot, recursive: true);
                }
            }
            catch
            {
                // Best-effort cleanup for temp repository roots.
            }
        }
    }
}
