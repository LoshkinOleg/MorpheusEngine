# Idea: storylets with exact state conditions, written by a PlotMaster

Date: 2026-11-06. Status: idea the owner wants to pursue, not yet designed or scheduled. Brainstorm it with the owner before writing a spec. The architecture spec lists the plot-consolidation agent ("PlotMaster") under "Not in v1"; this is a concrete shape for it.

## Goal
- Give runs direction (threads that develop, escalate and resolve) without a human authoring a plot.
- The engine decides deterministically, from the typed state, when a piece of story happens.
- A model writes the pieces, but never decides when they fire.

## Concept
A storylet is a small, self-contained piece of story that carries its own conditions. The engine checks every storylet against the current state. The ones whose conditions hold are eligible, one is chosen, and its effects change the state. That in turn changes which storylets are eligible next. Order emerges from state; nothing points to anything else. It is a different shape from a branching tree.

Example (sandcrawler):
```
id:       rationing_dispute
when:     crawler.water < 20 and crawler.morale != "mutinous"
effects:  crawler.morale = "low"
content:  Two crew chiefs confront the captain over the water ration.

id:       open_mutiny
when:     crawler.morale == "low" and crawler.water < 10
effects:  crawler.morale = "mutinous"
content:  Armed crew seize the water tanks.
```
Nobody wrote "the mutiny follows the dispute". If the player finds water, the mutiny never qualifies.

## Why it fits this engine
- **Conditions** are expressions over typed state paths, which the engine evaluates exactly. There is no model call and no text matching. Drama Llama and Dramamancer instead need a model to judge plain-language triggers every turn.
- **Effects** are ordinary engine changes, validated against the schema like the resolve call's.
- **Content** is a direction for the narrator, not authored prose: it reaches the narrate call through the resolution record, so the model still writes the text.
- **Choice among eligible storylets** uses the run's seeded RNG, so a turn stays reproducible and regenerate keeps it.
- **Fired storylets** are recorded in the state (e.g. a `story` entity), so snapshots, undo and regenerate handle them for free.

## Sketch
- **Format.** A storylet has: id, conditions, effects, content (a direction), plus optional `repeatable`/cooldown and priority.
- **Where they come from.** Two sources, in the same format:
  - a world's own file (`storylets.json`, hand-written, optional);
  - the run, written by the PlotMaster and stored in the run's database, since generated storylets belong to one run.
- **Condition language.** Small and typed:
  - comparisons on state paths (`==`, `!=`, `<`, `<=`, `>`, `>=`);
  - "holds"/"lacks" for lists and counters;
  - `and`/`or`/`not`.
  World-file conditions are validated against the schema at load: an unknown path, or an operator that does not fit the field's type, is a DataError.
- **When it runs in a turn** (to decide):
  - (a) After apply, before narrate. The turn's changes can trigger a storylet; its effects are applied and validated; its content is added to the resolution record. The narrator describes the outcome and the storylet together.
  - (b) Before resolve. The eligible storylet's content is shown to the resolve call as a situation, and the model proposes changes with it in mind.
  - (a) keeps the decision with the engine; (b) gives the model room to react. Possibly both: (a) for the effects, with the content visible to the next turn's resolve.
- **Budget.** Content adds to the narrate message, so it counts against `narrate_allowance`, with a cap per storylet. Nothing goes into the cached start of the prompt.
- **The PlotMaster.**
  - It is a model call outside the turn: at run start, and then as maintenance between turns, like folding.
  - At run start it works top-down (Dramatron-style): premise and threads from the world, then a few storylets per thread.
  - Between turns it adds storylets for threads that have stalled or resolved, and retires ones that can no longer fire.
  - Its output is grammar-constrained the same way `BuildResolveGrammar` is: conditions may only name paths and enum values that exist, and effects may only be valid changes.
- **Inspector.** A "Story" window, or a Turn log section: the eligible storylets, the one chosen and why, and the threads and their storylets.

## Related work in the repo
- **Ideas:**
  - Emily Short, "Storylets You Want Them" and "Beyond Branching" (docs/Papers, blog)
  - Kreminski & Wardrip-Fruin, "Sketching a Map of the Storylets Design Space"
  - Dan Cox on quality-based narrative (Failbetter)
- **Model-driven storylets:**
  - Drama Llama (Sun et al.)
  - Dramamancer (Wang et al.)
- **Generating structure:**
  - Dramatron (Mirowski et al.)
  - StoryVerse (Wang et al.)
  - Can LLMs Generate Good Stories (Wang & Kreminski)
- **Drama management background:**
  - Riedl & Bulitko
  - Roberts & Isbell
- **Code:** docs/references/freytag-forge: `storylets.md`, `storylet-routes.yaml`, `pacing.yaml` (authored storylets with exact fact operations and pacing windows).
- **Shared machinery:** the condition evaluator overlaps with "retrieve lore from the current state", the third section of the 2026-11-06 implementation brief in `.superpowers/briefs/`. Build one evaluator for both.

## Open questions
1. Should storylets fire after apply (a), be offered before resolve (b), or both?
2. How many storylets per turn at most, and what selection policy: priority, then seeded random? Salience (the most specific conditions win)?
3. Should the PlotMaster generate storylets at run start, during play, or both? How many live storylets at once?
4. Should there be hand-written world storylets in the first version, or generated ones only?
5. How does the player experience threads: invisible, or shown as hooks in the Play window?
6. Should storylets have pacing windows (freytag-forge's earliest/target/latest turn), or only state conditions?
