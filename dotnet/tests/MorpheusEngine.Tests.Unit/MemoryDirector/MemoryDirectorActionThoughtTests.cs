using System.Net;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine;
using MorpheusEngine.Tests.Unit.Helpers;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;

namespace MorpheusEngine.Tests.Unit.MemoryDirector;

[Trait("Category", "Unit")]
public sealed class MemoryDirectorActionThoughtTests
{
    private static readonly string REPOSITORY_ROOT = Path.GetFullPath(
        Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", ".."));

    // Verifies that thinking:false omits thought from the Ollama format schema required list.
    [Fact]
    public void MemoryDirector_LoadActionSchema_WhenThinkingDisabled_OmitsThoughtFromRequired()
    {
        var schema = MemoryDirectorType.LoadActionSchemaFromRepository(REPOSITORY_ROOT, requireActionThought: false);
        schema.GetProperty("required").EnumerateArray().Select(property => property.GetString()).Should().NotContain("thought");
        schema.TryGetProperty("properties", out var properties).Should().BeTrue();
        properties.TryGetProperty("thought", out _).Should().BeFalse();
    }

    // Verifies that thinking:true keeps thought required in the format schema.
    [Fact]
    public void MemoryDirector_LoadActionSchema_WhenThinkingEnabled_RequiresThought()
    {
        var schema = MemoryDirectorType.LoadActionSchemaFromRepository(REPOSITORY_ROOT, requireActionThought: true);
        schema.GetProperty("required").EnumerateArray().Select(property => property.GetString()).Should().Contain("thought");
    }

    // Verifies parse accepts tool-only JSON when thinking is disabled.
    [Fact]
    public void MemoryDirector_TryParseAction_WhenThinkingDisabled_AcceptsActionWithoutThought()
    {
        const string json = """{"tool":"generate_prose","arguments":{"message":"You look around."}}""";
        var ok = MemoryDirectorType.TryParseAction(json, requireActionThought: false, out var action, out var error);
        ok.Should().BeTrue();
        error.Should().BeEmpty();
        action.Thought.Should().BeEmpty();
        action.Tool.Should().Be("generate_prose");
    }

    // Verifies parse still requires thought when thinking is enabled.
    [Fact]
    public void MemoryDirector_TryParseAction_WhenThinkingEnabled_RequiresThought()
    {
        const string json = """{"tool":"generate_prose","arguments":{"message":"You look around."}}""";
        var ok = MemoryDirectorType.TryParseAction(json, requireActionThought: true, out _, out var error);
        ok.Should().BeFalse();
        error.Should().Be("Expected string property 'thought'.");
    }

    // Verifies compile-time system guidance omits thought instructions when thinking is disabled.
    [Fact]
    public async Task MemoryDirector_CompileContextAsync_WhenThinkingDisabled_OmitsThoughtPromptAndAddsNoThoughtRule()
    {
        var configuration = new TestConfigBuilder(REPOSITORY_ROOT)
            .AddModule("router", 19200)
            .AddModule(
                "llm_provider_qwen",
                19201,
                genericLlmProviderOptions: new GenericLlmProviderModuleOptions(4096),
                qwenOptions: new QwenModuleOptions(19212, "qwen2.5:7b", Thinking: false))
            .AddAlias("generic_llm_provider", "llm_provider_qwen")
            .Build();

        var mockHandler = new MockHttpHandler();
        mockHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                var proxyBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                using var document = JsonDocument.Parse(proxyBody);
                var targetPath = document.RootElement.GetProperty("targetPath").GetString();
                if (string.Equals(targetPath, "/token_count", StringComparison.Ordinal))
                {
                    return new HttpResponseMessage(HttpStatusCode.OK)
                    {
                        Content = new StringContent(
                            JsonSerializer.Serialize(new TokenCountResponse(true, "qwen2.5:7b", 50, false)),
                            Encoding.UTF8,
                            "application/json")
                    };
                }

                return new HttpResponseMessage(HttpStatusCode.NotFound);
            });

        var director = new MemoryDirectorType(configuration, new HttpClient(mockHandler));
        director.SetAgentPromptForTesting("Line one.\nKeep private reasoning brief in `thought`. Line two.\n");

        var compiled = await director.CompileContextAsync(
            new MemoryLoadContextResponse(
                true,
                [],
                [],
                new LatestSnapshotDto(1, """{"location":"camp"}""", """{"narration":"Quiet."}"""),
                new MemoryBudgetDto(4096, 2867, 12, 4000),
                null));

        var systemContent = compiled.Messages[0].Content;
        systemContent.Should().NotContain("Keep private reasoning brief in `thought`.");
        systemContent.Should().Contain("no thought field");
    }
}
