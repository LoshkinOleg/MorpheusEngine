using System.Text.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.App;

[Trait("Category", "Unit")]
public sealed class ContextInspectorPayloadViewTests
{
    [Fact]
    // Verifies that chat payloads keep only messages and drop Ollama transport and schema fields.
    public void Serialize_Chat_KeepsMessagesOnly()
    {
        using var document = JsonDocument.Parse(
            """
            {
              "model": "qwen2.5:7b-instruct",
              "messages": [
                { "role": "system", "content": "GM rules." },
                { "role": "user", "content": "Look around." }
              ],
              "stream": false,
              "truncate": false,
              "options": { "num_ctx": 8192, "num_keep": -1 },
              "format": { "type": "object" },
              "keep_alive": "30m"
            }
            """);

        var view = ContextInspectorOllamaPayloadView.Serialize(document.RootElement, "chat");
        using var parsed = JsonDocument.Parse(view);

        parsed.RootElement.TryGetProperty("model", out _).Should().BeFalse();
        parsed.RootElement.TryGetProperty("format", out _).Should().BeFalse();
        parsed.RootElement.TryGetProperty("options", out _).Should().BeFalse();
        parsed.RootElement.GetProperty("messages").GetArrayLength().Should().Be(2);
        parsed.RootElement.GetProperty("messages")[1].GetProperty("content").GetString().Should().Be("Look around.");
    }

    [Fact]
    // Verifies that generate payloads keep prompt and system only.
    public void Serialize_Generate_KeepsPromptAndSystem()
    {
        using var document = JsonDocument.Parse(
            """
            {
              "model": "qwen2.5:7b-instruct",
              "prompt": "Say hi.",
              "system": "You are helpful.",
              "stream": false,
              "options": { "num_ctx": 8192 }
            }
            """);

        var view = ContextInspectorOllamaPayloadView.Serialize(document.RootElement, "generate");
        using var parsed = JsonDocument.Parse(view);

        parsed.RootElement.GetProperty("prompt").GetString().Should().Be("Say hi.");
        parsed.RootElement.GetProperty("system").GetString().Should().Be("You are helpful.");
        parsed.RootElement.TryGetProperty("model", out _).Should().BeFalse();
        parsed.RootElement.TryGetProperty("options", out _).Should().BeFalse();
    }
}
