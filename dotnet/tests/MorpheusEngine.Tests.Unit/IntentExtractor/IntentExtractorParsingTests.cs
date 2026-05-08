using System.Text.Json;
using FluentAssertions;
using IntentExtractorType = global::MorpheusEngine.IntentExtractor;

namespace MorpheusEngine.Tests.Unit.IntentExtractor;

[Trait("Category", "Unit")]
public sealed class IntentExtractorParsingTests
{
    // Verifies that a clean JSON string is returned unchanged.
    [Fact]
    public void IntentExtractor_ExtractJsonObject_CleanJsonString_ReturnsJson()
    {
        const string raw = """{"intent":"wait","params":{}}""";

        var result = IntentExtractorType.ExtractJsonObject(raw);

        result.Should().Be(raw);
    }

    // Verifies that fenced JSON is extracted without Markdown fences.
    [Fact]
    public void IntentExtractor_ExtractJsonObject_FencedJson_StripsMarkdownFences()
    {
        const string raw =
            """
            ```json
            {"intent":"inspect","params":{"target":"door"}}
            ```
            """;

        var result = IntentExtractorType.ExtractJsonObject(raw);

        result.Should().Be("""{"intent":"inspect","params":{"target":"door"}}""");
    }

    // Verifies that JSON embedded in prose is extracted correctly.
    [Fact]
    public void IntentExtractor_ExtractJsonObject_ProseWrappedJson_ExtractsJsonObject()
    {
        const string raw = """I found the intent. {"intent":"wait","params":{}} Let me know if you need anything else.""";

        var result = IntentExtractorType.ExtractJsonObject(raw);

        result.Should().Be("""{"intent":"wait","params":{}}""");
    }

    // Verifies that missing JSON content returns null.
    [Fact]
    public void IntentExtractor_ExtractJsonObject_NoJsonObject_ReturnsNull()
    {
        var result = IntentExtractorType.ExtractJsonObject("No structured content here.");

        result.Should().BeNull();
    }

    // Verifies that valid intent JSON parses and normalizes parameters.
    [Fact]
    public void IntentExtractor_TryParseIntentResult_ValidIntentAndParams_Succeeds()
    {
        var result = IntentExtractorType.TryParseIntentResult(
            """{"intent":" inspect ","params":{"target":"airlock","count":2}}""",
            out var extraction);

        result.Should().BeTrue();
        extraction.Should().NotBeNull();
        extraction!.Intent.Should().Be("inspect");
        extraction.Parameters.Should().ContainKey("target").WhoseValue.Should().Be("airlock");
        extraction.Parameters.Should().ContainKey("count").WhoseValue.Should().Be("2");
    }

    // Verifies that the unsupported parameters alias is rejected.
    [Fact]
    public void IntentExtractor_TryParseIntentResult_ParametersAlias_Fails()
    {
        var result = IntentExtractorType.TryParseIntentResult(
            """{"intent":"inspect","parameters":{"target":"door"}}""",
            out var extraction);

        result.Should().BeFalse();
        extraction.Should().BeNull();
    }

    // Verifies that parsing fails when the intent field is missing.
    [Fact]
    public void IntentExtractor_TryParseIntentResult_MissingIntent_Fails()
    {
        var result = IntentExtractorType.TryParseIntentResult(
            """{"params":{"target":"door"}}""",
            out var extraction);

        result.Should().BeFalse();
        extraction.Should().BeNull();
    }

    // Verifies that parsing fails when the intent field is blank.
    [Fact]
    public void IntentExtractor_TryParseIntentResult_EmptyIntent_Fails()
    {
        var result = IntentExtractorType.TryParseIntentResult(
            """{"intent":"   ","params":{"target":"door"}}""",
            out var extraction);

        result.Should().BeFalse();
        extraction.Should().BeNull();
    }

    // Verifies that parsing fails when params is not a JSON object.
    [Fact]
    public void IntentExtractor_TryParseIntentResult_NonObjectParams_Fails()
    {
        var result = IntentExtractorType.TryParseIntentResult(
            """{"intent":"inspect","params":"door"}""",
            out var extraction);

        result.Should().BeFalse();
        extraction.Should().BeNull();
    }

    // Verifies that known intent name variants normalize to canonical names.
    [Theory]
    [InlineData("INSPECT", "inspect")]
    [InlineData(" move_self ", "move_self")]
    [InlineData("Wait", "wait")]
    [InlineData("FREEFORM_ACTION", "freeform_action")]
    public void IntentExtractor_NormalizeIntentName_KnownVariants_ReturnCanonicalIntent(string rawIntent, string canonicalIntent)
    {
        var result = IntentExtractorType.NormalizeIntentName(rawIntent);

        result.Should().Be(canonicalIntent);
    }

    // Verifies that unknown intent names do not normalize.
    [Fact]
    public void IntentExtractor_NormalizeIntentName_UnknownIntent_ReturnsNull()
    {
        var result = IntentExtractorType.NormalizeIntentName("dance");

        result.Should().BeNull();
    }

    // Verifies that inspect intents require a non-empty target parameter.
    [Fact]
    public void IntentExtractor_TryNormalizeAndValidateIntent_InspectWithoutTarget_Fails()
    {
        var parsed = new IntentExtractorType.IntentExtractionResult(
            "inspect",
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase));

        var result = IntentExtractorType.TryNormalizeAndValidateIntent(parsed, out var valid, out var error);

        result.Should().BeFalse();
        valid.Should().BeNull();
        error.Should().Be("Intent 'inspect' requires a non-empty params.target.");
    }

    // Verifies that move_self intents validate after canonical normalization.
    [Fact]
    public void IntentExtractor_TryNormalizeAndValidateIntent_MoveSelfWithTarget_Succeeds()
    {
        var parsed = new IntentExtractorType.IntentExtractionResult(
            "MOVE_SELF",
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["target"] = "north"
            });

        var result = IntentExtractorType.TryNormalizeAndValidateIntent(parsed, out var valid, out var error);

        result.Should().BeTrue();
        error.Should().BeNull();
        valid.Should().NotBeNull();
        valid!.Intent.Should().Be("move_self");
        valid.Parameters.Should().ContainKey("target").WhoseValue.Should().Be("north");
    }

    // Verifies that freeform_action intents require a non-empty text parameter.
    [Fact]
    public void IntentExtractor_TryNormalizeAndValidateIntent_FreeformActionWithoutText_Fails()
    {
        var parsed = new IntentExtractorType.IntentExtractionResult(
            "freeform_action",
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase));

        var result = IntentExtractorType.TryNormalizeAndValidateIntent(parsed, out var valid, out var error);

        result.Should().BeFalse();
        valid.Should().BeNull();
        error.Should().Be("Intent 'freeform_action' requires a non-empty params.text.");
    }

    // Verifies that wait intents validate successfully without parameters.
    [Fact]
    public void IntentExtractor_TryNormalizeAndValidateIntent_WaitWithoutParams_Succeeds()
    {
        var parsed = new IntentExtractorType.IntentExtractionResult(
            "wait",
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase));

        var result = IntentExtractorType.TryNormalizeAndValidateIntent(parsed, out var valid, out var error);

        result.Should().BeTrue();
        error.Should().BeNull();
        valid.Should().NotBeNull();
        valid!.Intent.Should().Be("wait");
        valid.Parameters.Should().BeEmpty();
    }

    // Verifies that scalar JSON values are flattened into string map entries.
    [Fact]
    public void IntentExtractor_CopyStringMap_Scalars_FlattensValuesToStrings()
    {
        using var document = JsonDocument.Parse(
            """{"text":"airlock","count":12,"precise":42.5,"armed":true,"hostile":false,"note":null}""");
        var destination = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        IntentExtractorType.CopyStringMap(document.RootElement, destination);

        destination.Should().Contain(new KeyValuePair<string, string>("text", "airlock"));
        destination.Should().Contain(new KeyValuePair<string, string>("count", "12"));
        destination.Should().Contain(new KeyValuePair<string, string>("precise", "42.5"));
        destination.Should().Contain(new KeyValuePair<string, string>("armed", "True"));
        destination.Should().Contain(new KeyValuePair<string, string>("hostile", "False"));
        destination.Should().Contain(new KeyValuePair<string, string>("note", string.Empty));
    }
}
