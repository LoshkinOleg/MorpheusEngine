using FluentAssertions;
using RouterType = global::MorpheusEngine.Router;

namespace MorpheusEngine.Tests.Unit.Router;

[Trait("Category", "Unit")]
public sealed class RouterHelpersTests
{
    [Fact]
    public void Router_TruncateForLog_ShortString_PassesThrough()
    {
        var text = "short text";

        var result = RouterType.TruncateForLog(text);

        result.Should().Be(text);
    }

    [Fact]
    public void Router_TruncateForLog_LongString_TruncatesAndAppendsEllipsis()
    {
        var text = new string('a', 200);

        var result = RouterType.TruncateForLog(text, maxLen: 10);

        result.Should().Be("aaaaaaaaaa…");
    }

    [Fact]
    public void Router_TruncateMiddle_LongText_KeepsHeadAndTailWithMiddleEllipsis()
    {
        const string text = "abcdefghijklmnopqrstuvwxyz";

        var result = RouterType.TruncateMiddle(text, headChars: 5, tailChars: 4);

        result.Should().Be("abcde ... wxyz");
    }

    [Fact]
    public void Router_TruncateMiddle_ShortText_PassesThrough()
    {
        const string text = "short text";

        var result = RouterType.TruncateMiddle(text, headChars: 5, tailChars: 4);

        result.Should().Be(text);
    }

    [Fact]
    public void Router_IsPersistTurnStep_ReturnsTrueOnlyForSessionStorePostPersistTurn()
    {
        var persistStep = new EngineTurnPipelineStepInfo(
            "persist_turn",
            "session_store",
            "/persist_turn",
            "POST",
            "{}");
        var wrongModule = new EngineTurnPipelineStepInfo(
            "persist_turn",
            "memory_director",
            "/persist_turn",
            "POST",
            "{}");
        var wrongMethod = new EngineTurnPipelineStepInfo(
            "persist_turn",
            "session_store",
            "/persist_turn",
            "GET",
            "{}");
        var wrongPath = new EngineTurnPipelineStepInfo(
            "persist_turn",
            "session_store",
            "/message",
            "POST",
            "{}");

        RouterType.IsPersistTurnStep(persistStep).Should().BeTrue();
        RouterType.IsPersistTurnStep(wrongModule).Should().BeFalse();
        RouterType.IsPersistTurnStep(wrongMethod).Should().BeFalse();
        RouterType.IsPersistTurnStep(wrongPath).Should().BeFalse();
    }
}
