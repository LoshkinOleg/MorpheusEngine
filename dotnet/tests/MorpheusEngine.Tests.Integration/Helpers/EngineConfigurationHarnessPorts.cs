using MorpheusEngine;

namespace MorpheusEngine.Tests.Integration.Helpers;

internal static class EngineConfigurationHarnessPorts
{
    public static int GetOutboundOllamaPortForEmbeddings(EngineConfiguration configuration)
    {
        var module = configuration.FindModule("embeddings_ollama")
            ?? throw new InvalidOperationException("Engine configuration missing embeddings_ollama module.");
        return module.EmbeddingsOptions?.OllamaPort
            ?? throw new InvalidOperationException(
                "embeddings_ollama module must expose EmbeddingsModuleOptions.OllamaPort for integration mocks.");
    }

    public static int GetOutboundOllamaPortForLlmProviderQwen(EngineConfiguration configuration)
    {
        var module = configuration.FindModule("llm_provider_qwen")
            ?? throw new InvalidOperationException("Engine configuration missing llm_provider_qwen module.");
        return module.QwenOptions?.OllamaPort
            ?? throw new InvalidOperationException(
                "llm_provider_qwen module must expose QwenModuleOptions.OllamaPort for integration mocks.");
    }
}
