using System.Net;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using MorpheusEngine.Tests.Unit.Helpers;
using MemoryDirectorType = global::MorpheusEngine.MemoryDirector;

namespace MorpheusEngine.Tests.Unit.MemoryDirector;

[Trait("Category", "Unit")]
public sealed class MemoryDirectorContextCompilationTests
{
    // Verifies that compiled context includes the prompt, core blocks, snapshot, and recent messages.
    [Fact]
    public async Task MemoryDirector_CompileContextAsync_IncludesAgentPromptCoreBlocksSnapshotAndRecentMessages()
    {
        using var harness = CreateDirectorHarness(
            static _ => new TokenCountResponse(true, "qwen2.5:7b", 250, false),
            "You are a careful memory-managed director.");

        var compiled = await harness.Director.CompileContextAsync(CreateMemoryContext(
            blocks:
            [
                CreateMemoryBlock("player", value: "Carries a lantern."),
                CreateMemoryBlock("current_scene", value: "A sealed bronze airlock blocks the path.")
            ],
            recentMessages:
            [
                CreateAgentMessage(1, 0, "assistant", "You stand still and listen."),
                CreateAgentMessage(1, 1, "player", "inspect the airlock", messageType: "player_input")
            ],
            latestSnapshot: new LatestSnapshotDto(
                2,
                """{"location":"airlock","door":"sealed"}""",
                """{"narration":"The corridor hums softly."}""")));

        compiled.Messages.Should().HaveCount(3);
        compiled.Messages[0].Role.Should().Be("system");
        compiled.Messages[0].Content.Should().Contain("You are a careful memory-managed director.");
        compiled.Messages[0].Content.Should().Contain("Core memory blocks:");
        compiled.Messages[0].Content.Should().Contain("[player]");
        compiled.Messages[0].Content.Should().Contain("Carries a lantern.");
        compiled.Messages[0].Content.Should().Contain("""{"location":"airlock","door":"sealed"}""");
        compiled.Messages[0].Content.Should().Contain("""{"narration":"The corridor hums softly."}""");
        compiled.Messages[1].Role.Should().Be("assistant");
        compiled.Messages[1].Content.Should().Be("You stand still and listen.");
        compiled.Messages[2].Role.Should().Be("user");
        compiled.Messages[2].Content.Should().Be("inspect the airlock");
    }

    // Verifies that messages are omitted when the character budget is exceeded.
    [Fact]
    public async Task MemoryDirector_CompileContextAsync_CharacterBudget_OmitsMessagesWhenExceeded()
    {
        using var harness = CreateDirectorHarness(
            static _ => new TokenCountResponse(true, "qwen2.5:7b", 300, false),
            "Short prompt.");

        var recentMessages = new[]
        {
            CreateAgentMessage(1, 0, "assistant", new string('A', 550)),
            CreateAgentMessage(1, 1, "assistant", new string('B', 550)),
            CreateAgentMessage(1, 2, "assistant", new string('C', 550))
        };

        var compiled = await harness.Director.CompileContextAsync(CreateMemoryContext(
            blocks: [],
            recentMessages: recentMessages,
            budget: new MemoryBudgetDto(4096, 50, 12, 4000)));

        compiled.Messages.Count.Should().BeLessThan(recentMessages.Length + 1);
        compiled.Accounting.Omissions.Should().Contain(label => label.StartsWith("message:", StringComparison.Ordinal));
        compiled.Accounting.Items.Should().Contain(item =>
            item.Type == "recent_message"
            && (item.Status == "truncated" || item.Status == "omitted"));
    }

    // Verifies that compiled context includes summaries when they are present.
    [Fact]
    public async Task MemoryDirector_CompileContextAsync_IncludesSummariesWhenPresent()
    {
        using var harness = CreateDirectorHarness(
            static _ => new TokenCountResponse(true, "qwen2.5:7b", 250, false),
            "Short prompt.");

        var compiled = await harness.Director.CompileContextAsync(CreateMemoryContext(
            blocks: [],
            recentMessages: [],
            summaries:
            [
                new MemorySummaryDto(1, 3, "The player crossed the desert and found the airlock.", 4)
            ]));

        compiled.Messages[0].Content.Should().Contain("Compacted recall summaries:");
        compiled.Messages[0].Content.Should().Contain("Turns 1-3: The player crossed the desert and found the airlock.");
    }

    // Verifies that consuming budget returns the available amount and reduces the remainder.
    [Fact]
    public void MemoryDirector_ContextBudget_Consume_ReturnsMinimumAndDecrementsRemaining()
    {
        var budget = new MemoryDirectorType.ContextBudget(10);

        var first = budget.Consume(6);
        var second = budget.Consume(8);

        first.Should().Be(6);
        budget.TargetChars.Should().Be(10);
        second.Should().Be(4);
        budget.RemainingChars.Should().Be(0);
    }

    // Verifies that exact token budget enforcement removes trailing messages.
    [Fact]
    public async Task MemoryDirector_CompileContextAsync_ExactTokenBudget_RemovesTrailingMessages()
    {
        using var harness = CreateDirectorHarness(
            static text => new TokenCountResponse(true, "qwen2.5:7b", text.Length, true),
            "Short prompt.");

        var firstMessage = new string('A', 220);
        var secondMessage = new string('B', 220);
        var thirdMessage = new string('C', 220);

        var compiled = await harness.Director.CompileContextAsync(CreateMemoryContext(
            blocks: [],
            recentMessages:
            [
                CreateAgentMessage(1, 0, "assistant", firstMessage),
                CreateAgentMessage(1, 1, "assistant", secondMessage),
                CreateAgentMessage(1, 2, "assistant", thirdMessage)
            ],
            budget: new MemoryBudgetDto(4096, 900, 12, 4000)));

        compiled.Messages.Select(message => message.Content).Should().Contain(firstMessage);
        compiled.Messages.Select(message => message.Content).Should().NotContain(thirdMessage);
        compiled.Accounting.Items.Should().Contain(item =>
            item.Label == "message:1:2:assistant"
            && item.Status == "omitted"
            && item.Reason == "exact_token_budget");
    }

    private static DirectorHarness CreateDirectorHarness(
        Func<string, TokenCountResponse> tokenCountFactory,
        string agentPrompt)
    {
        var mockHandler = new MockHttpHandler();
        mockHandler.On(
            "POST",
            "/proxy",
            request =>
            {
                var proxyBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                using var document = JsonDocument.Parse(proxyBody);
                var proxyRequest = document.RootElement;
                var targetPath = proxyRequest.GetProperty("targetPath").GetString();

                if (string.Equals(targetPath, "/token_count", StringComparison.Ordinal))
                {
                    var text = proxyRequest.GetProperty("body").GetProperty("text").GetString() ?? string.Empty;
                    return BuildJsonHttpResponse(HttpStatusCode.OK, tokenCountFactory(text));
                }

                return BuildJsonHttpResponse(
                    HttpStatusCode.NotFound,
                    new ErrorResponse(false, "Unexpected proxy path for unit test.", targetPath));
            });

        var repositoryRoot = GetRepositoryRoot();
        var configuration = new TestConfigBuilder(repositoryRoot)
            .AddModule("router", 19100)
            .AddModule(
                "llm_provider_qwen",
                19101,
                genericLlmProviderOptions: new GenericLlmProviderModuleOptions(4096),
                qwenOptions: new QwenModuleOptions(19112, "qwen2.5:7b"))
            .AddAlias("generic_llm_provider", "llm_provider_qwen")
            .Build();

        var director = new MemoryDirectorType(configuration, new HttpClient(mockHandler));
        director.SetAgentPromptForTesting(agentPrompt);
        return new DirectorHarness(director);
    }

    private static MemoryLoadContextResponse CreateMemoryContext(
        IReadOnlyList<MemoryBlockDto> blocks,
        IReadOnlyList<AgentMessageDto> recentMessages,
        LatestSnapshotDto? latestSnapshot = null,
        MemoryBudgetDto? budget = null,
        IReadOnlyList<MemorySummaryDto>? summaries = null)
    {
        return new MemoryLoadContextResponse(
            true,
            blocks,
            recentMessages,
            latestSnapshot ?? new LatestSnapshotDto(
                1,
                """{"location":"camp"}""",
                """{"narration":"The fire crackles softly."}"""),
            budget ?? new MemoryBudgetDto(4096, 2867, 12, 4000),
            summaries);
    }

    private static MemoryBlockDto CreateMemoryBlock(
        string label,
        string description = "Stable player-facing facts.",
        string value = "Player prefers concise descriptions.",
        int charLimit = 2000,
        bool readOnly = false)
    {
        return new MemoryBlockDto(label, description, value, charLimit, readOnly);
    }

    private static AgentMessageDto CreateAgentMessage(
        int turn,
        int stepNumber,
        string role,
        string content,
        string messageType = "send_message",
        string? toolName = null,
        string? toolCallId = null)
    {
        return new AgentMessageDto(turn, stepNumber, role, messageType, content, toolName, toolCallId);
    }

    private static HttpResponseMessage BuildJsonHttpResponse<T>(HttpStatusCode statusCode, T payload)
    {
        return new HttpResponseMessage(statusCode)
        {
            Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json")
        };
    }

    private static string GetRepositoryRoot()
    {
        return Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                "..",
                "..",
                "..",
                "..",
                "..",
                ".."));
    }

    private sealed class DirectorHarness : IDisposable
    {
        public DirectorHarness(MemoryDirectorType director)
        {
            Director = director;
        }

        public MemoryDirectorType Director { get; }

        public void Dispose()
        {
            Director.RequestShutdown();
        }
    }
}
