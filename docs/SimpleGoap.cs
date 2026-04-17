// SimpleGoap.cs — a minimal Goal-Oriented Action Planner in ~150 lines.
// Teaching implementation. Uses forward A* search (easier to read than
// regressive search, which is what Orkin's original F.E.A.R. GOAP used).
//
// Four components:
//   1. WorldState    — a set of named boolean facts (predicates).
//   2. GoapAction    — preconditions, effects, cost.
//   3. GoapPlanner   — A* search through state space to satisfy a goal.
//   4. A scenario    — demonstrates all of the above.

using System;
using System.Collections.Generic;
using System.Linq;

namespace SimpleGoap;

// ---------------------------------------------------------------------------
// World state: a dictionary of named boolean facts.
// Production GOAP uses bitfields for speed; dicts are clearer for teaching.
// ---------------------------------------------------------------------------
public sealed class WorldState
{
    private readonly Dictionary<string, bool> _facts;

    public WorldState() => _facts = new();
    public WorldState(Dictionary<string, bool> facts) => _facts = new(facts);

    public bool Get(string key) => _facts.TryGetValue(key, out var v) && v;

    public WorldState With(IReadOnlyDictionary<string, bool> changes)
    {
        var next = new Dictionary<string, bool>(_facts);
        foreach (var (k, v) in changes) next[k] = v;
        return new WorldState(next);
    }

    public bool Satisfies(IReadOnlyDictionary<string, bool> requirements)
    {
        foreach (var (k, v) in requirements)
            if (Get(k) != v) return false;
        return true;
    }

    // Canonical signature for the A* visited-set.
    public string Signature()
        => string.Join(",", _facts.OrderBy(kv => kv.Key)
                                  .Select(kv => $"{kv.Key}={kv.Value}"));
}

// ---------------------------------------------------------------------------
// Action: pure data. Name, preconditions, effects, cost.
// The planner decides when (or whether) to use each one.
// ---------------------------------------------------------------------------
public sealed class GoapAction
{
    public string Name { get; }
    public IReadOnlyDictionary<string, bool> Preconditions { get; }
    public IReadOnlyDictionary<string, bool> Effects { get; }
    public float Cost { get; }

    public GoapAction(
        string name,
        IReadOnlyDictionary<string, bool> preconditions,
        IReadOnlyDictionary<string, bool> effects,
        float cost = 1f)
    {
        Name = name;
        Preconditions = preconditions;
        Effects = effects;
        Cost = cost;
    }

    public bool CanRun(WorldState state) => state.Satisfies(Preconditions);
    public WorldState Apply(WorldState state) => state.With(Effects);

    public override string ToString() => Name;
}

// ---------------------------------------------------------------------------
// Planner: forward A* over state space.
// Expand every applicable action, track visited states, return cheapest plan.
// ---------------------------------------------------------------------------
public static class GoapPlanner
{
    private sealed class Node
    {
        public WorldState State = default!;
        public GoapAction? Action;   // action that produced this state
        public Node? Parent;
        public float Cost;            // g(n): accumulated cost from start
    }

    public static List<GoapAction>? Plan(
        WorldState start,
        IReadOnlyDictionary<string, bool> goal,
        IEnumerable<GoapAction> actions)
    {
        var root = new Node { State = start };

        // A proper min-heap (PriorityQueue<Node,float>) is more efficient.
        // Sorted list keeps the teaching version readable.
        var open    = new List<Node> { root };
        var visited = new HashSet<string> { start.Signature() };

        while (open.Count > 0)
        {
            open.Sort((a, b) => a.Cost.CompareTo(b.Cost));
            var current = open[0];
            open.RemoveAt(0);

            if (current.State.Satisfies(goal))
                return Reconstruct(current);

            foreach (var action in actions)
            {
                if (!action.CanRun(current.State)) continue;

                var next = action.Apply(current.State);
                var sig  = next.Signature();
                if (!visited.Add(sig)) continue;

                open.Add(new Node
                {
                    State  = next,
                    Action = action,
                    Parent = current,
                    Cost   = current.Cost + action.Cost,
                });
            }
        }

        return null; // no plan exists
    }

    private static List<GoapAction> Reconstruct(Node end)
    {
        var plan = new List<GoapAction>();
        for (var n = end; n.Action != null; n = n.Parent!)
            plan.Add(n.Action);
        plan.Reverse();
        return plan;
    }
}

// ---------------------------------------------------------------------------
// Example: a scout at ruined site needs to get intel to the captain.
// Two viable paths; planner picks the cheaper one.
// ---------------------------------------------------------------------------
public static class Example
{
    public static void Run()
    {
        var start = new WorldState(new Dictionary<string, bool>
        {
            ["atRuins"]         = true,
            ["atCrawler"]       = false,
            ["hasIntel"]        = false,
            ["radioWorking"]    = false,
            ["captainHasIntel"] = false,
        });

        var goal = new Dictionary<string, bool>
        {
            ["captainHasIntel"] = true,
        };

        var actions = new[]
        {
            new GoapAction("GatherIntel",
                preconditions: new Dictionary<string, bool> { ["atRuins"] = true },
                effects:       new Dictionary<string, bool> { ["hasIntel"] = true },
                cost: 1f),

            new GoapAction("FixRadio",
                preconditions: new Dictionary<string, bool> { ["atRuins"] = true },
                effects:       new Dictionary<string, bool> { ["radioWorking"] = true },
                cost: 3f),

            new GoapAction("Broadcast",
                preconditions: new Dictionary<string, bool>
                    { ["radioWorking"] = true, ["hasIntel"] = true },
                effects:       new Dictionary<string, bool> { ["captainHasIntel"] = true },
                cost: 1f),

            new GoapAction("TravelToCrawler",
                preconditions: new Dictionary<string, bool> { ["atRuins"] = true },
                effects:       new Dictionary<string, bool>
                    { ["atRuins"] = false, ["atCrawler"] = true },
                cost: 5f),

            new GoapAction("ReportInPerson",
                preconditions: new Dictionary<string, bool>
                    { ["atCrawler"] = true, ["hasIntel"] = true },
                effects:       new Dictionary<string, bool> { ["captainHasIntel"] = true },
                cost: 1f),
        };

        var plan = GoapPlanner.Plan(start, goal, actions);

        if (plan is null)
        {
            Console.WriteLine("No plan found.");
            return;
        }

        Console.WriteLine($"Plan ({plan.Count} steps):");
        foreach (var a in plan) Console.WriteLine($"  -> {a.Name,-16} (cost {a.Cost})");
        Console.WriteLine($"Total cost: {plan.Sum(a => a.Cost)}");

        // Expected output:
        //   Plan (3 steps):
        //     -> GatherIntel      (cost 1)
        //     -> FixRadio         (cost 3)
        //     -> Broadcast        (cost 1)
        //   Total cost: 5
        //
        // The alternative (GatherIntel -> TravelToCrawler -> ReportInPerson)
        // costs 7. A* returns the cheaper one.
    }
}
