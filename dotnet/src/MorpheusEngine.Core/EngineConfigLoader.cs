using System.Text.Json;
using System.Text.Json.Serialization;

namespace MorpheusEngine;

public sealed record EnginePorts(int Router, int LlmProviderQwen, int IntentExtractor, int Ollama);

public sealed record EngineModuleLaunchInfo(string Artifact, string? DevProject);

public sealed record EngineEndpointInfo(
    string Path,
    string? Description,
    string Method,
    string? RequestContract,
    string? BodyTemplate);

public sealed record EngineModuleInfo(
    string PortKey,
    string DisplayName,
    bool Required,
    EngineModuleLaunchInfo Launch,
    IReadOnlyList<EngineEndpointInfo> Endpoints);

public sealed record EngineConfiguration(
    string RepositoryRoot,
    EnginePorts Ports,
    IReadOnlyList<EngineModuleInfo> Modules)
{
    public int? ResolvePort(string portKey) => portKey switch
    {
        "router" => Ports.Router,
        "llm_provider_qwen" => Ports.LlmProviderQwen,
        "intent_extractor" => Ports.IntentExtractor,
        "ollama" => Ports.Ollama,
        _ => null
    };

    public string DotnetRoot
    {
        get
        {
            var dotnetRoot = Path.Combine(RepositoryRoot, "dotnet");
            return Directory.Exists(dotnetRoot) ? dotnetRoot : RepositoryRoot;
        }
    }

    public EngineModuleInfo? FindModule(string portKey)
    {
        foreach (var module in Modules)
        {
            if (string.Equals(module.PortKey, portKey, StringComparison.OrdinalIgnoreCase))
            {
                return module;
            }
        }

        return null;
    }

    public EngineModuleInfo? GetModuleForListeningPort(int port)
    {
        foreach (var module in Modules)
        {
            if (ResolvePort(module.PortKey) == port)
            {
                return module;
            }
        }

        return null;
    }

    public EngineEndpointInfo? FindEndpointForPort(int port, string path)
    {
        var normalized = NormalizePath(path);
        var module = GetModuleForListeningPort(port);
        if (module is null)
        {
            return null;
        }

        foreach (var endpoint in module.Endpoints)
        {
            if (string.Equals(NormalizePath(endpoint.Path), normalized, StringComparison.OrdinalIgnoreCase))
            {
                return endpoint;
            }
        }

        return null;
    }

    public static string NormalizePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return "/";
        }

        var trimmed = path.Trim();
        return trimmed.StartsWith('/') ? trimmed : "/" + trimmed;
    }
}

public static class EngineConfigLoader
{
    private const int DefaultRouter = 8790;
    private const int DefaultLlmProviderQwen = 8791;
    private const int DefaultIntentExtractor = 8792;
    private const int DefaultOllama = 11434;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true
    };

    private static EngineConfiguration? _cached;
    private static readonly object Sync = new();

    public static EngineConfiguration GetConfiguration()
    {
        if (_cached is not null)
        {
            return _cached;
        }

        lock (Sync)
        {
            return _cached ??= LoadConfigurationUncached();
        }
    }

    public static EnginePorts GetPorts() => GetConfiguration().Ports;

    private static EngineConfiguration LoadConfigurationUncached()
    {
        var repositoryRoot = FindRepositoryRoot();
        var path = repositoryRoot is null ? null : Path.Combine(repositoryRoot, "engine_config.json");
        if (repositoryRoot is null || path is null || !File.Exists(path))
        {
            Console.WriteLine("engine_config.json not found; using default engine configuration.");
            return BuildDefaultConfiguration(repositoryRoot ?? Environment.CurrentDirectory);
        }

        try
        {
            var json = File.ReadAllText(path);
            var dto = JsonSerializer.Deserialize<EngineConfigFileDto>(json, JsonOptions);
            var ports = MergePorts(dto?.Ports, path);
            var modules = MergeModules(dto?.Modules, ports, repositoryRoot, path);
            Console.WriteLine($"Loaded engine configuration from {path}.");
            return new EngineConfiguration(repositoryRoot, ports, modules);
        }
        catch (Exception e)
        {
            Console.WriteLine("Failed to read engine_config.json; using defaults. " + e.Message);
            return BuildDefaultConfiguration(repositoryRoot);
        }
    }

    private static EnginePorts MergePorts(PortsDto? ports, string path)
    {
        var router = NormalizePort(ports?.Router, DefaultRouter, "router");
        var qwen = NormalizePort(ports?.LlmProviderQwen, DefaultLlmProviderQwen, "llm_provider_qwen");
        var intentExtractor = NormalizePort(ports?.IntentExtractor, DefaultIntentExtractor, "intent_extractor");
        var ollama = NormalizePort(ports?.Ollama, DefaultOllama, "ollama");
        Console.WriteLine($"Engine ports: router={router}, llm_provider_qwen={qwen}, intent_extractor={intentExtractor}, ollama={ollama} ({path})");
        return new EnginePorts(router, qwen, intentExtractor, ollama);
    }

    private static IReadOnlyList<EngineModuleInfo> MergeModules(
        List<ModuleDto>? modulesDto,
        EnginePorts ports,
        string repositoryRoot,
        string path)
    {
        if (modulesDto is null || modulesDto.Count == 0)
        {
            Console.WriteLine("No modules in engine_config.json; using built-in fallback module configuration.");
            return CreateBuiltinModules(repositoryRoot);
        }

        var list = new List<EngineModuleInfo>();
        foreach (var module in modulesDto)
        {
            var portKey = module.PortKey?.Trim();
            if (string.IsNullOrEmpty(portKey))
            {
                Console.WriteLine("Skipping module with missing port_key.");
                continue;
            }

            if (ResolvePortKey(ports, portKey) is null)
            {
                Console.WriteLine($"Unknown port_key '{portKey}'; skipping module.");
                continue;
            }

            var launch = MergeLaunch(module.Launch, portKey);
            if (launch is null)
            {
                Console.WriteLine($"Module '{portKey}' is missing valid launch metadata; skipping.");
                continue;
            }

            var endpoints = MergeEndpoints(module.Endpoints);
            list.Add(new EngineModuleInfo(
                portKey,
                string.IsNullOrWhiteSpace(module.DisplayName) ? portKey : module.DisplayName.Trim(),
                module.Required ?? true,
                launch,
                endpoints));
        }

        if (list.Count == 0)
        {
            Console.WriteLine($"No valid modules in engine_config.json ({path}); using built-in fallback module configuration.");
            return CreateBuiltinModules(repositoryRoot);
        }

        return list;
    }

    private static EngineModuleLaunchInfo? MergeLaunch(ModuleLaunchDto? launch, string portKey)
    {
        if (launch is null || string.IsNullOrWhiteSpace(launch.Artifact))
        {
            return null;
        }

        return new EngineModuleLaunchInfo(
            launch.Artifact.Trim(),
            string.IsNullOrWhiteSpace(launch.DevProject) ? null : launch.DevProject.Trim());
    }

    private static IReadOnlyList<EngineEndpointInfo> MergeEndpoints(List<EndpointDto>? endpointsDto)
    {
        var list = new List<EngineEndpointInfo>();
        if (endpointsDto is null)
        {
            return list;
        }

        foreach (var endpoint in endpointsDto)
        {
            var path = endpoint.Path?.Trim();
            if (string.IsNullOrEmpty(path))
            {
                continue;
            }

            var method = string.IsNullOrWhiteSpace(endpoint.Method)
                ? InferMethod(endpoint.RequestContract, endpoint.BodyTemplate)
                : endpoint.Method.Trim().ToUpperInvariant();

            if (method != "GET" && method != "POST")
            {
                method = InferMethod(endpoint.RequestContract, endpoint.BodyTemplate);
            }

            var requestContract = string.IsNullOrWhiteSpace(endpoint.RequestContract)
                ? null
                : endpoint.RequestContract.Trim();

            list.Add(new EngineEndpointInfo(
                EngineConfiguration.NormalizePath(path),
                string.IsNullOrWhiteSpace(endpoint.Description) ? null : endpoint.Description.Trim(),
                method,
                requestContract,
                string.IsNullOrWhiteSpace(endpoint.BodyTemplate)
                    ? EngineContractExamples.TryGetRequestBodyTemplate(requestContract)
                    : endpoint.BodyTemplate));
        }

        return list;
    }

    private static string InferMethod(string? requestContract, string? bodyTemplate) =>
        string.IsNullOrWhiteSpace(requestContract) && string.IsNullOrWhiteSpace(bodyTemplate)
            ? "GET"
            : "POST";

    private static int? ResolvePortKey(EnginePorts ports, string portKey) => portKey switch
    {
        "router" => ports.Router,
        "llm_provider_qwen" => ports.LlmProviderQwen,
        "intent_extractor" => ports.IntentExtractor,
        "ollama" => ports.Ollama,
        _ => null
    };

    private static int NormalizePort(int? value, int fallback, string name)
    {
        if (value is >= 1 and <= 65535)
        {
            return value.Value;
        }

        if (value is not null)
        {
            Console.WriteLine($"Invalid port for '{name}' ({value}); using default {fallback}.");
        }

        return fallback;
    }

    private static EnginePorts Defaults() => new(DefaultRouter, DefaultLlmProviderQwen, DefaultIntentExtractor, DefaultOllama);

    private static EngineConfiguration BuildDefaultConfiguration(string repositoryRoot) =>
        new(repositoryRoot, Defaults(), CreateBuiltinModules(repositoryRoot));

    private static IReadOnlyList<EngineModuleInfo> CreateBuiltinModules(string repositoryRoot) =>
        new[]
        {
            new EngineModuleInfo(
                "router",
                "RouterModule",
                true,
                new EngineModuleLaunchInfo(
                    @"dotnet/src/MorpheusEngine.RouterModule/bin/Debug/net9.0/MorpheusEngine.RouterModule.exe",
                    @"dotnet/src/MorpheusEngine.RouterModule/MorpheusEngine.RouterModule.csproj"),
                new[]
                {
                    new EngineEndpointInfo("/info", "Module metadata", "GET", null, null),
                    new EngineEndpointInfo("/health", "Health check", "GET", null, null),
                    new EngineEndpointInfo("/turn", "Submit player turn", "POST", "turn_request",
                        EngineContractExamples.TryGetRequestBodyTemplate("turn_request"))
                }),
            new EngineModuleInfo(
                "llm_provider_qwen",
                "LlmProvider_qwen",
                true,
                new EngineModuleLaunchInfo(
                    @"dotnet/src/MorpheusEngine.LlmProvider_qwen/bin/Debug/net9.0/MorpheusEngine.LlmProvider_qwen.exe",
                    @"dotnet/src/MorpheusEngine.LlmProvider_qwen/MorpheusEngine.LlmProvider_qwen.csproj"),
                new[]
                {
                    new EngineEndpointInfo("/info", "Module metadata", "GET", null, null),
                    new EngineEndpointInfo("/health", "Health check", "GET", null, null),
                    new EngineEndpointInfo("/generate", "LLM generate", "POST", "qwen_generate_request",
                        EngineContractExamples.TryGetRequestBodyTemplate("qwen_generate_request"))
                }),
            new EngineModuleInfo(
                "intent_extractor",
                "IntentExtractor",
                true,
                new EngineModuleLaunchInfo(
                    @"dotnet/src/MorpheusEngine.IntentExtractor/bin/Debug/net9.0/MorpheusEngine.IntentExtractor.exe",
                    @"dotnet/src/MorpheusEngine.IntentExtractor/MorpheusEngine.IntentExtractor.csproj"),
                new[]
                {
                    new EngineEndpointInfo("/info", "Module metadata", "GET", null, null),
                    new EngineEndpointInfo("/health", "Health check", "GET", null, null),
                    new EngineEndpointInfo("/intent", "Extract player intent", "POST", "intent_request",
                        EngineContractExamples.TryGetRequestBodyTemplate("intent_request"))
                })
        };

    public static string? FindRepositoryRoot()
    {
        return FindRepositoryRoot(new DirectoryInfo(AppContext.BaseDirectory))
            ?? FindRepositoryRoot(new DirectoryInfo(Environment.CurrentDirectory));
    }

    private static string? FindRepositoryRoot(DirectoryInfo? start)
    {
        for (var dir = start; dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "engine_config.json")))
            {
                return dir.FullName;
            }

            if (File.Exists(Path.Combine(dir.FullName, "dotnet", "MorpheusEngine.sln")))
            {
                return dir.FullName;
            }

            if (File.Exists(Path.Combine(dir.FullName, "MorpheusEngine.sln")))
            {
                return dir.Parent?.FullName ?? dir.FullName;
            }
        }

        return null;
    }

    private sealed class EngineConfigFileDto
    {
        public PortsDto? Ports { get; set; }
        public List<ModuleDto>? Modules { get; set; }
    }

    private sealed class ModuleDto
    {
        [JsonPropertyName("port_key")]
        public string? PortKey { get; set; }

        [JsonPropertyName("display_name")]
        public string? DisplayName { get; set; }

        public bool? Required { get; set; }
        public ModuleLaunchDto? Launch { get; set; }
        public List<EndpointDto>? Endpoints { get; set; }
    }

    private sealed class ModuleLaunchDto
    {
        public string? Artifact { get; set; }

        [JsonPropertyName("dev_project")]
        public string? DevProject { get; set; }
    }

    private sealed class EndpointDto
    {
        public string? Path { get; set; }
        public string? Description { get; set; }
        public string? Method { get; set; }

        [JsonPropertyName("request_contract")]
        public string? RequestContract { get; set; }

        [JsonPropertyName("body_template")]
        public string? BodyTemplate { get; set; }
    }

    private sealed class PortsDto
    {
        [JsonPropertyName("router")]
        public int? Router { get; set; }

        [JsonPropertyName("llm_provider_qwen")]
        public int? LlmProviderQwen { get; set; }

        [JsonPropertyName("intent_extractor")]
        public int? IntentExtractor { get; set; }

        [JsonPropertyName("ollama")]
        public int? Ollama { get; set; }
    }
}
