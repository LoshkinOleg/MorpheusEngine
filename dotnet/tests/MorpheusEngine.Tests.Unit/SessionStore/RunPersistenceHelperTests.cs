using System.Text.Json;
using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.SessionStore;

[Trait("Category", "Unit")]
public sealed class RunPersistenceHelperTests
{
    [Fact]
    public void RunPersistence_CosineSimilarity_IdenticalVectors_ReturnsOne()
    {
        var result = RunPersistence.CosineSimilarity([1f, 2f, 3f], [1f, 2f, 3f]);

        result.Should().BeApproximately(1.0, 1e-9);
    }

    [Fact]
    public void RunPersistence_CosineSimilarity_OrthogonalVectors_ReturnsZero()
    {
        var result = RunPersistence.CosineSimilarity([1f, 0f], [0f, 1f]);

        result.Should().BeApproximately(0.0, 1e-9);
    }

    [Fact]
    public void RunPersistence_CosineSimilarity_OppositeVectors_ReturnsNegativeOne()
    {
        var result = RunPersistence.CosineSimilarity([1f, 0f], [-1f, 0f]);

        result.Should().BeApproximately(-1.0, 1e-9);
    }

    [Fact]
    public void RunPersistence_CosineSimilarity_MismatchedDimensions_ThrowsInvalidOperationException()
    {
        var act = () => RunPersistence.CosineSimilarity([1f, 2f], [1f]);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*matching dimensions*");
    }

    [Fact]
    public void RunPersistence_CosineSimilarity_EmptyVectors_ThrowsInvalidOperationException()
    {
        var act = () => RunPersistence.CosineSimilarity([], []);

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*non-empty vectors*");
    }

    [Fact]
    public void RunPersistence_CosineSimilarity_ZeroMagnitudeVector_ReturnsZero()
    {
        var result = RunPersistence.CosineSimilarity([0f, 0f], [1f, 2f]);

        result.Should().Be(0.0);
    }

    [Fact]
    public void RunPersistence_BuildViewStateEnvelope_ValidJson_WrapsDirectorResponse()
    {
        var result = RunPersistence.BuildViewStateEnvelope("""{"ok":true,"text":"The door creaks open."}""");

        using var document = JsonDocument.Parse(result);
        var directorResponse = document.RootElement.GetProperty("directorResponse");
        directorResponse.GetProperty("ok").GetBoolean().Should().BeTrue();
        directorResponse.GetProperty("text").GetString().Should().Be("The door creaks open.");
    }

    [Fact]
    public void RunPersistence_BuildViewStateEnvelope_InvalidJson_WrapsDirectorRawText()
    {
        var result = RunPersistence.BuildViewStateEnvelope("non-json response text");

        using var document = JsonDocument.Parse(result);
        document.RootElement.GetProperty("directorRawText").GetString().Should().Be("non-json response text");
    }

    [Fact]
    public void RunPersistence_BuildModuleTracePayload_ProducesExpectedJsonStructure()
    {
        var result = RunPersistence.BuildModuleTracePayload(
            "look around",
            """{"ok":true,"text":"You stand still and listen."}""");

        using var document = JsonDocument.Parse(result);
        document.RootElement.GetProperty("narrationText").GetString().Should().Be("You stand still and listen.");
        document.RootElement.GetProperty("directorRaw").GetString().Should().Be("""{"ok":true,"text":"You stand still and listen."}""");
        document.RootElement.GetProperty("playerInputEcho").GetString().Should().Be("look around");
    }

    [Fact]
    public void RunPersistence_BuildFtsQuery_SanitizesSpecialCharactersAndJoinsTermsWithOr()
    {
        var result = RunPersistence.BuildFtsQuery(""" ancient, ruins!!! temple@door """);

        result.Should().Be("\"ancient\" OR \"ruins\" OR \"templedoor\"");
    }

    [Fact]
    public void RunPersistence_BuildFtsQuery_WhitespaceOnlyInput_ReturnsEmptyQueryFallback()
    {
        var result = RunPersistence.BuildFtsQuery("   ");

        result.Should().Be("\"\"");
    }

    [Fact]
    public void RunPersistence_CreateStableArchivalId_SameInputs_IsDeterministic()
    {
        var first = RunPersistence.CreateStableArchivalId("lore/default_lore_entries.csv", "Ancient Ruins");
        var second = RunPersistence.CreateStableArchivalId("lore/default_lore_entries.csv", "Ancient Ruins");

        first.Should().Be(second);
    }

    [Fact]
    public void RunPersistence_CreateStableArchivalId_DifferentInputs_ProduceDifferentIds()
    {
        var first = RunPersistence.CreateStableArchivalId("lore/default_lore_entries.csv", "Ancient Ruins");
        var second = RunPersistence.CreateStableArchivalId("lore/default_lore_entries.csv", "Oasis City");

        first.Should().NotBe(second);
    }

    [Fact]
    public void RunPersistence_PayloadMatchesEventType_MatchingEventType_ReturnsTrue()
    {
        var result = RunPersistence.PayloadMatchesEventType("""{"eventType":"memory_context_budget","accounting":{}}""", "memory_context_budget");

        result.Should().BeTrue();
    }

    [Fact]
    public void RunPersistence_PayloadMatchesEventType_NullOrEmptyEventType_MatchesAnything()
    {
        RunPersistence.PayloadMatchesEventType("""{"eventType":"memory_context_budget"}""", null).Should().BeTrue();
        RunPersistence.PayloadMatchesEventType("""{"eventType":"memory_context_budget"}""", string.Empty).Should().BeTrue();
        RunPersistence.PayloadMatchesEventType("""{"eventType":"memory_context_budget"}""", "   ").Should().BeTrue();
    }

    [Fact]
    public void RunPersistence_PayloadMatchesEventType_MalformedJson_ReturnsFalse()
    {
        var result = RunPersistence.PayloadMatchesEventType("{not json", "memory_context_budget");

        result.Should().BeFalse();
    }

    [Fact]
    public void RunPersistence_ValidateArchivalPassage_ValidPassage_DoesNotThrow()
    {
        var act = () => RunPersistence.ValidateArchivalPassage(CreateArchivalPassage());

        act.Should().NotThrow();
    }

    [Fact]
    public void RunPersistence_ValidateArchivalPassage_InvalidFields_Throw()
    {
        var emptyIdAct = () => RunPersistence.ValidateArchivalPassage(CreateArchivalPassage(id: " "));
        var badScopeAct = () => RunPersistence.ValidateArchivalPassage(CreateArchivalPassage(scope: "invalid"));
        var badDimensionsAct = () => RunPersistence.ValidateArchivalPassage(new ArchivalPassageDto(
            "passage-001",
            "project",
            "lore/default_lore_entries.csv",
            "Ancient ruins contain sealed northern doors.",
            ["lore", "seed"],
            """{"subject":"Ancient Ruins"}""",
            "nomic-embed-text",
            4,
            [0.12f, -0.04f, 0.88f]));

        emptyIdAct.Should().Throw<InvalidOperationException>().WithMessage("Archival passage id must be non-empty.");
        badScopeAct.Should().Throw<InvalidOperationException>().WithMessage("*scope must be either 'project' or 'run'*");
        badDimensionsAct.Should().Throw<InvalidOperationException>().WithMessage("*embeddingDimensions must match*");
    }

    private static ArchivalPassageDto CreateArchivalPassage(
        string id = "passage-001",
        string scope = "project",
        string source = "lore/default_lore_entries.csv",
        string content = "Ancient ruins contain sealed northern doors.",
        IReadOnlyList<string>? tags = null,
        string? metadataJson = """{"subject":"Ancient Ruins"}""",
        string embeddingModel = "nomic-embed-text",
        IReadOnlyList<float>? embedding = null)
    {
        var vector = embedding ?? [0.12f, -0.04f, 0.88f];
        return new ArchivalPassageDto(
            id,
            scope,
            source,
            content,
            tags ?? ["lore", "seed"],
            metadataJson,
            embeddingModel,
            vector.Count,
            vector);
    }
}
