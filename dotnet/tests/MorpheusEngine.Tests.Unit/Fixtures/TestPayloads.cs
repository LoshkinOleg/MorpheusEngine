using System.Text.Json;

namespace MorpheusEngine.Tests.Unit.Fixtures;

internal static class TestPayloads
{
    public const string MinimalManifestJson = """
        {
          "id": "test_game",
          "title": "Test Game",
          "required_modules": ["generic_director", "generic_llm_provider", "session_store"],
          "turn_pipeline": "memory_director_default"
        }
        """;

    public const string MinimalLoreCsv = """
        subject,data
        Ancient Ruins,"Crumbling structures in the northern desert."
        Oasis City,"A walled settlement around a freshwater spring."
        """;

    public const string MinimalSystemInstructions = """
        You are the game master for a focused test scenario.
        Respond clearly and keep the scene moving.
        """;

    public static string BuildMinimalEngineConfigJson()
    {
        var config = new
        {
            module_aliases = new Dictionary<string, string>
            {
                ["generic_llm_provider"] = "llm_provider_qwen",
                ["generic_director"] = "memory_director",
                ["generic_embeddings"] = "embeddings_ollama"
            },
            turn_pipelines = new Dictionary<string, object>
            {
                ["memory_director_default"] = new
                {
                    steps = new object[]
                    {
                        new
                        {
                            id = "director_message",
                            target_module = "generic_director",
                            path = "/message",
                            method = "POST",
                            body_template = "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}}}"
                        },
                        new
                        {
                            id = "persist_turn",
                            target_module = "session_store",
                            path = "/persist_turn",
                            method = "POST",
                            body_template = "{\"turn\":{{turn}},\"playerInput\":{{playerInputJson}},\"directorResponseBody\":{{step.director_message.rawBodyJson}}}"
                        }
                    },
                    response_mapping = new
                    {
                        source_step = "director_message",
                        type = "director_message_response"
                    }
                }
            },
            modules = new object[]
            {
                new
                {
                    port_key = "router",
                    port = 19100,
                    load_order = 10,
                    display_name = "Router",
                    required_by_engine = true,
                    launch = "router.dll",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/turn", description = "Turn", method = "POST", template_contracts_id = "turn" },
                        new { path = "/proxy", description = "Proxy", method = "POST", template_contracts_id = "proxy" }
                    }
                },
                new
                {
                    port_key = "memory_director",
                    port = 19101,
                    load_order = 20,
                    display_name = "MemoryDirector",
                    required_by_engine = true,
                    launch = "memory_director.dll",
                    max_steps_per_turn = 12,
                    max_tool_result_chars = 4000,
                    max_full_messages = 12,
                    keep_model_loaded_for = "30m",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/message", description = "Message", method = "POST", template_contracts_id = "director_message" }
                    }
                },
                new
                {
                    port_key = "llm_provider_qwen",
                    port = 19102,
                    load_order = 30,
                    display_name = "LLM Provider",
                    required_by_engine = true,
                    launch = "llm_provider_qwen.dll",
                    num_ctx = 4096,
                    ollama_port = 19112,
                    default_chat_model = "qwen2.5:7b",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/chat", description = "Chat", method = "POST", template_contracts_id = "chat" },
                        new { path = "/generate", description = "Generate", method = "POST", template_contracts_id = "generate" }
                    }
                },
                new
                {
                    port_key = "embeddings_ollama",
                    port = 19103,
                    load_order = 40,
                    display_name = "Embeddings",
                    required_by_engine = true,
                    launch = "embeddings_ollama.dll",
                    ollama_port = 19112,
                    default_embedding_model = "nomic-embed-text",
                    keep_model_loaded_for = "30m",
                    embeddings_num_ctx = 2048,
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/embed", description = "Embed", method = "POST", template_contracts_id = "embeddings" }
                    }
                },
                new
                {
                    port_key = "session_store",
                    port = 19104,
                    load_order = 50,
                    display_name = "Session Store",
                    required_by_engine = false,
                    launch = "session_store.dll",
                    endpoints = new object[]
                    {
                        new { path = "/health", description = "Health", method = "GET", template_contracts_id = "module_health" },
                        new { path = "/initialize", description = "Initialize", method = "POST", template_contracts_id = "initialize" },
                        new { path = "/persist_turn", description = "Persist Turn", method = "POST", template_contracts_id = "persist_turn" }
                    }
                }
            }
        };

        return JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true });
    }
}
