using FluentAssertions;

namespace MorpheusEngine.Tests.Integration.Helpers;

// Unit-style tests for the harness teardown collector. The collector itself is internal to the
// Integration test assembly because it's only consumed by harnesses living in the same assembly,
// so the tests live here too. The Unit category trait keeps them alongside the rest of the fast
// unit suite for CI selection.
[Trait("Category", "Unit")]
public sealed class HarnessTeardownErrorCollectorTests
{
    [Fact]
    public void ThrowIfAny_NoStepsRegistered_DoesNotThrow()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");

        var act = () => collector.ThrowIfAny();

        act.Should().NotThrow();
        collector.HasFailures.Should().BeFalse();
    }

    [Fact]
    public void ThrowIfAny_AllStepsSucceeded_DoesNotThrow()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");
        collector.Run("step.one", () => { });
        collector.Run("step.two", () => { });

        collector.HasFailures.Should().BeFalse();
        Action act = collector.ThrowIfAny;
        act.Should().NotThrow();
    }

    [Fact]
    public async Task RunAsync_AllStepsRunInOrder_EvenWhenAnEarlierOneThrows()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");
        var executionOrder = new List<string>();

        await collector.RunAsync("step.alpha", () =>
        {
            executionOrder.Add("alpha");
            return Task.CompletedTask;
        });
        await collector.RunAsync("step.beta", () =>
        {
            executionOrder.Add("beta");
            throw new InvalidOperationException("beta failed");
        });
        await collector.RunAsync("step.gamma", () =>
        {
            executionOrder.Add("gamma");
            return Task.CompletedTask;
        });

        executionOrder.Should().Equal("alpha", "beta", "gamma");
        collector.HasFailures.Should().BeTrue();
    }

    [Fact]
    public void Run_AllStepsRunInOrder_EvenWhenAnEarlierOneThrows()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");
        var executionOrder = new List<string>();

        collector.Run("step.alpha", () => executionOrder.Add("alpha"));
        collector.Run("step.beta", () =>
        {
            executionOrder.Add("beta");
            throw new InvalidOperationException("beta failed");
        });
        collector.Run("step.gamma", () => executionOrder.Add("gamma"));

        executionOrder.Should().Equal("alpha", "beta", "gamma");
        collector.HasFailures.Should().BeTrue();
    }

    [Fact]
    public void ThrowIfAny_OneStepFailed_ThrowsAggregateExceptionCarryingTheLabel()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");
        collector.Run("step.alpha", () => { });
        collector.Run("step.beta", () => throw new InvalidOperationException("beta exploded"));
        collector.Run("step.gamma", () => { });

        Action act = collector.ThrowIfAny;

        var aggregate = act.Should().Throw<AggregateException>().Which;
        aggregate.Message.Should().Contain("TestHarness teardown failed");
        aggregate.Message.Should().Contain("step.beta");
        aggregate.InnerExceptions.Should().ContainSingle()
            .Which.Should().BeOfType<InvalidOperationException>()
            .Which.Message.Should().Be("beta exploded");
    }

    [Fact]
    public async Task ThrowIfAny_MultipleStepsFailed_AggregateCarriesAllInRegistrationOrder()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");
        collector.Run("step.alpha", () => throw new InvalidOperationException("alpha failed"));
        await collector.RunAsync("step.beta", () => Task.CompletedTask);
        collector.Run("step.gamma", () => throw new ArgumentException("gamma failed"));

        Action act = collector.ThrowIfAny;

        var aggregate = act.Should().Throw<AggregateException>().Which;
        aggregate.InnerExceptions.Should().HaveCount(2);
        aggregate.InnerExceptions[0].Should().BeOfType<InvalidOperationException>();
        aggregate.InnerExceptions[0].Data[HarnessTeardownErrorCollector.STEP_LABEL_DATA_KEY]
            .Should().Be("step.alpha");
        aggregate.InnerExceptions[1].Should().BeOfType<ArgumentException>();
        aggregate.InnerExceptions[1].Data[HarnessTeardownErrorCollector.STEP_LABEL_DATA_KEY]
            .Should().Be("step.gamma");
        aggregate.Message.Should().Contain("step.alpha, step.gamma");
    }

    [Fact]
    public async Task RunAsync_StepDelegateThrowsSynchronously_StillCaught()
    {
        var collector = new HarnessTeardownErrorCollector("TestHarness");

        // A delegate that throws BEFORE returning a Task must be treated the same as a
        // delegate that returns a faulted Task; otherwise the collector's "always run all
        // steps" guarantee would only hold for one of the two failure modes.
        await collector.RunAsync("step.sync_throw", () =>
            throw new InvalidOperationException("sync throw"));

        collector.HasFailures.Should().BeTrue();
        Action act = collector.ThrowIfAny;
        act.Should().Throw<AggregateException>()
            .Which.InnerExceptions.Should().ContainSingle()
            .Which.Should().BeOfType<InvalidOperationException>();
    }

    [Fact]
    public void Constructor_NullOrWhitespaceHarnessName_Throws()
    {
        var actNull = () => new HarnessTeardownErrorCollector(null!);
        var actWhitespace = () => new HarnessTeardownErrorCollector("   ");

        actNull.Should().Throw<ArgumentException>();
        actWhitespace.Should().Throw<ArgumentException>();
    }
}
