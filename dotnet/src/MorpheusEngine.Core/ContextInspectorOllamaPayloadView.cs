using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

/// <summary>
/// Builds a Context Inspector view from the Ollama wire JSON stored on GET /debug/last_llm_payload.
/// Keeps only fields that carry prompt text to the model; drops transport and schema envelope fields.
/// </summary>
public static class ContextInspectorOllamaPayloadView
{
    /// <summary>
    /// Chat: messages only. Generate: prompt and system only.
    /// Drops model, stream, truncate, options, format, keep_alive, think.
    /// </summary>
    public static string Serialize(JsonElement ollamaPayload, string endpoint)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            if (string.Equals(endpoint, "generate", StringComparison.OrdinalIgnoreCase))
            {
                WriteStringPropertyIfPresent(ollamaPayload, writer, "prompt");
                WriteStringPropertyIfPresent(ollamaPayload, writer, "system");
            }
            else if (ollamaPayload.TryGetProperty("messages", out var messages))
            {
                writer.WritePropertyName("messages");
                messages.WriteTo(writer);
            }

            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static void WriteStringPropertyIfPresent(JsonElement source, Utf8JsonWriter writer, string propertyName)
    {
        if (!source.TryGetProperty(propertyName, out var value) || value.ValueKind != JsonValueKind.String)
        {
            return;
        }

        writer.WriteString(propertyName, value.GetString());
    }
}
