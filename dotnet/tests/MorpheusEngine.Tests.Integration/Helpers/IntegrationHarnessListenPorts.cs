namespace MorpheusEngine.Tests.Integration.Helpers;

// Canonical loopback ports for Fixtures/Configurations JSON literals and EngineConfigLoader validation.
//
// Constraints:
// - Integration assembly disables parallel collections in xunit.runner.json; sequential runs reuse primary numbers safely.
// - Concurrent dual-stack tests (alternate LLM beside Qwen stack) use integration_end_to_end_alternate ENGINE_CONFIG SECONDARY listen block 59110-59159.
// - If another local process occupies the reserved spans, binds fail loudly — unlike probe-then-release races.
// - MorpheusEngineCoreIntegrationTests and TestModuleHost sandboxes intentionally use ephemeral ports outside this band.
internal static class IntegrationHarnessListenPorts
{
    public const int ROUTER_LISTEN = 59010;

    /// <summary>Director-only fixtures (exclusive with memory-director-slot fixtures across sequential runs).</summary>
    public const int DIRECTOR_LISTEN = 59020;

    /// <summary>Memory director listens on the director-family slot.</summary>
    public const int MEMORY_DIRECTOR_LISTEN = 59020;

    /// <summary>Intent extractor listens between director and session to keep the six-slot layout unique.</summary>
    public const int INTENT_EXTRACTOR_LISTEN = 59022;

    public const int SESSION_STORE_LISTEN = 59030;

    public const int LLM_PROVIDER_LISTEN = 59040;

    /// <summary>Shares the LLM listen slot when only one alternate stack fixture is mounted at a time (not stacked with PRIMARY qwen).</summary>
    public const int ALTERNATE_LLM_PROVIDER_LISTEN = 59040;

    public const int EMBEDDINGS_OLLAMA_LISTEN = 59050;

    /// <summary>Routers binding second in-process stack (integration_end_to_end_alternate).</summary>
    public const int E2E_ALTERNATE_ROUTER_LISTEN = 59110;

    public const int E2E_ALTERNATE_MEMORY_DIRECTOR_LISTEN = 59120;

    public const int E2E_ALTERNATE_SESSION_STORE_LISTEN = 59130;

    public const int E2E_ALTERNATE_LLM_PROVIDER_LISTEN = 59140;

    public const int E2E_ALTERNATE_EMBEDDINGS_OLLAMA_LISTEN = 59150;

    /// <summary>Mock outbound Ollama for embeddings_ollama; must stay distinct from all listen ports above.</summary>
    public const int OLLAMA_EMBEDDINGS_MOCK_BACKEND = 59100;

    /// <summary>Mock outbound Ollama for llm_provider_qwen.</summary>
    public const int OLLAMA_LLM_QWEN_MOCK_BACKEND = 59101;
}
