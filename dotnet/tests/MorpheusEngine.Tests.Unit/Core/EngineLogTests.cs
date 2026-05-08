using System.Collections.Concurrent;
using System.Text;
using System.Text.RegularExpressions;
using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.Core;

[Trait("Category", "Unit")]
[Collection("EngineProcessState")]
public sealed class EngineLogTests : IDisposable
{
    private readonly TextWriter _originalOut = Console.Out;
    private readonly TextWriter _originalErr = Console.Error;

    [Fact]
    public void EngineLog_Initialize_AllowedSource_PrefixesSubsequentConsoleWrites()
    {
        using var stdout = new StringWriter(new StringBuilder());
        Console.SetOut(stdout);

        EngineLog.Initialize("App");
        Console.WriteLine("hello world");

        var output = stdout.ToString();
        output.Should().MatchRegex(@"^\[\d+ ; \d{2}:\d{2}:\d{2}::\d{2}\] \[App\] hello world\r?\n$");
    }

    [Fact]
    public void EngineLog_Initialize_DisallowedSource_ThrowsInvalidOperationException()
    {
        var act = () => EngineLog.Initialize("Router");

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*host-only*not allowed*");
    }

    [Fact]
    public void EngineLog_Initialize_CalledTwice_IsIdempotent()
    {
        using var stdout = new StringWriter(new StringBuilder());
        Console.SetOut(stdout);

        EngineLog.Initialize("App");
        var wrappedWriter = Console.Out;

        var act = () => EngineLog.Initialize("App");

        act.Should().NotThrow();
        ReferenceEquals(Console.Out, wrappedWriter).Should().BeTrue();

        Console.WriteLine("hello once");
        stdout.ToString().Should().Contain("[App] hello once");
        CountOccurrences(stdout.ToString(), "[App]").Should().Be(1);
    }

    [Fact]
    public void EngineLog_WriteHostedChildLine_BeforeInitialize_ThrowsInvalidOperationException()
    {
        var act = () => EngineLog.WriteHostedChildLine("child_module", isError: false, "hello");

        act.Should().Throw<InvalidOperationException>()
            .WithMessage("*not initialized*");
    }

    [Fact]
    public void EngineLog_PrefixingTextWriter_MultilineWriteString_PrefixesEachLineExactlyOnce()
    {
        using var stdout = new StringWriter(new StringBuilder());
        Console.SetOut(stdout);

        EngineLog.Initialize("App");
        Console.Write("alpha\nbeta\ngamma");

        var output = stdout.ToString();
        CountOccurrences(output, "[App]").Should().Be(3);

        var lines = SplitNonEmptyLines(output);
        lines.Should().HaveCount(3);
        lines.Should().AllSatisfy(line =>
        {
            line.Should().Contain("[App] ");
            CountOccurrences(line, "[App]").Should().Be(1);
        });
        lines[0].Should().EndWith("alpha");
        lines[1].Should().EndWith("beta");
        lines[2].Should().EndWith("gamma");
    }

    [Fact]
    public void EngineLog_PrefixingTextWriter_CharByCharAcrossLineBoundaries_PrefixesEachLineExactlyOnce()
    {
        using var stdout = new StringWriter(new StringBuilder());
        Console.SetOut(stdout);

        EngineLog.Initialize("App");
        foreach (var ch in "alpha\nbeta\ngamma")
        {
            Console.Write(ch);
        }

        var output = stdout.ToString();
        CountOccurrences(output, "[App]").Should().Be(3);

        var lines = SplitNonEmptyLines(output);
        lines.Should().HaveCount(3);
        lines.Should().AllSatisfy(line =>
        {
            line.Should().Contain("[App] ");
            CountOccurrences(line, "[App]").Should().Be(1);
        });
        lines[0].Should().EndWith("alpha");
        lines[1].Should().EndWith("beta");
        lines[2].Should().EndWith("gamma");
    }

    [Fact]
    public void EngineLog_BuildLinePrefix_AcrossThreads_ProducesMonotonicallyIncreasingSequenceIds()
    {
        const int iterations = 200;
        var prefixes = new ConcurrentBag<string>();

        Parallel.For(0, iterations, _ =>
        {
            prefixes.Add(EngineLog.BuildLinePrefix(isError: false, source: "App"));
        });

        var sequenceIds = prefixes
            .Select(ExtractSequenceId)
            .OrderBy(id => id)
            .ToArray();

        sequenceIds.Should().Equal(Enumerable.Range(1, iterations).Select(i => (ulong)i));
    }

    public void Dispose()
    {
        EngineLog.ResetForTesting();
        Console.SetOut(_originalOut);
        Console.SetError(_originalErr);
    }

    private static int CountOccurrences(string text, string needle)
    {
        return Regex.Matches(text, Regex.Escape(needle)).Count;
    }

    private static IReadOnlyList<string> SplitNonEmptyLines(string text)
    {
        return text
            .Split(["\r\n", "\n"], StringSplitOptions.None)
            .Where(line => line.Length > 0)
            .ToArray();
    }

    private static ulong ExtractSequenceId(string prefix)
    {
        var match = Regex.Match(prefix, @"^\[(\d+) ; ");
        match.Success.Should().BeTrue($"expected a sequence-bearing prefix but got '{prefix}'");
        return ulong.Parse(match.Groups[1].Value);
    }
}
