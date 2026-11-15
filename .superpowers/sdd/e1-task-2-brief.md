### Task 2: `SummaryScope`: the upkeep reads and writes summaries only through a scope

**Files:**
- Modify: `src/turn/Folding.h`, `src/turn/Folding.cpp` (instructions as parameters)
- Modify: `src/turn/MemoryUpkeep.h`, `src/turn/MemoryUpkeep.cpp`
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp` (`LayOut` knows where the summaries sit)
- Modify: `src/turn/Session.cpp` (the run-wide scope), `src/turn/HostedSession.cpp` (its scope)
- Test: `tests/turn/MemoryUpkeepTests.cpp`

**Interfaces:**
- Consumes: Task 1's store methods.
- Produces:
  - In `turn/MemoryUpkeep.h`:

```cpp
// Which summaries a fold and a condense work on (per-speaker memory spec, section 5): the owned game's run-wide
// summaries, or one NPC's in a hosted run. MemoryUpkeep reads and writes summaries only through it.
struct SummaryScope {
    // The scope's shown summaries, oldest first: what a condense replaces.
    std::function<std::vector<core::Summary>()> shown = {};
    // Stores a fold's summary (and makes its turns searchable history).
    std::function<void(const core::Summary& summary)> storeFold = {};
    // Replaces the shown summaries with `condensed`.
    std::function<void(const core::Summary& condensed)> storeCondensed = {};
    // The fold call's instruction.
    std::string foldInstruction = {};
    // The condense call's user message for `summaries` (the shown ones): the owned game's is the plain instruction (its
    // system message holds the summaries); a hosted one renders the memories before the instruction.
    std::function<std::string(const std::vector<core::Summary>& summaries)> condenseMessage = {};
};
```

  - `MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings, std::string tailName, int turn, int attempt, SummaryScope scope)` (replaces the `StoreFold` parameter; `StoreFold` goes).
  - `inference::Prompt SummarizePrompt(const inference::Message& systemMessage, const std::vector<context::TranscriptEntry>& entries, std::string_view instruction)`, `SummarizeTurns(systemMessage, entries, instruction, spec, backend, stop)`, `inference::Prompt CondensePrompt(const inference::Message& systemMessage, std::string_view message)`, `CondenseSummaries(systemMessage, summaries, message, spec, backend, stop)`.
  - `context::SummaryPlace { System, Tail }` in `context/PromptBuilder.h`; `LayOut` takes it (owned: `System`; hosted: `System` until Task 4 moves it).

- [ ] **Step 1: Write the failing test**

Add to `tests/turn/MemoryUpkeepTests.cpp` (read the file's helpers first; it builds a store, a scripted backend and an `Assemble` over transcript entries):

```cpp
TEST_CASE("MemoryUpkeep reads and writes summaries only through its scope, with the scope's instructions",
          "[turn][upkeep]")
{
    // A scope over a vector: folds and condenses land in it, never in the store, and the calls carry its texts.
    std::vector<core::Summary> kept = {{1, 2, "Earlier."}, {3, 4, "Later."}};
    turn::SummaryScope scope;
    scope.shown = [&kept] { return kept; };
    scope.storeFold = [&kept](const core::Summary& summary) { kept.push_back(summary); };
    scope.storeCondensed = [&kept](const core::Summary& condensed) { kept = {condensed}; };
    scope.foldInstruction = "Fold for the scope.";
    scope.condenseMessage = [](const std::vector<core::Summary>& summaries) {
        return std::format("Condense these {} for the scope.", summaries.size());
    };
    test::ScriptedBackend backend(16384, [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content.starts_with("Condense these") ? std::string("Condensed.")
                                                                           : std::string("Folded.");
    });
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_scope", 5, 8);

    // The prompt follows the vector: entries after its last summary are unfolded, and more than two summaries are over
    // the cap.
    turn::MemoryUpkeep::Assemble assemble = [&](const std::vector<turn::RetrievalHit>&) {
        const int foldedThrough = kept.empty() ? 0 : kept.back().toTurn;
        turn::FitPass pass;
        for (const context::TranscriptEntry& entry : harness.entries) {
            if (entry.turn > foldedThrough) {
                pass.transcript.push_back(entry);
            }
        }
        pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
        pass.assembly.prompt.stableCount = 1;
        pass.assembly.summaryOverBudget = kept.size() > 2;
        return pass;
    };
    turn::MemoryUpkeep upkeep(harness.store, backend, settings, "the test tail", 9, 0, scope);
    CHECK(upkeep.FoldAll(assemble, {}));

    // 5-8 fold in one call with the scope's instruction; three summaries over the cap condense into one with the
    // scope's message; the store's run-wide summaries are untouched.
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].fromTurn == 1);
    CHECK(kept[0].toTurn == 8);
    CHECK(kept[0].text == "Condensed.");
    REQUIRE(backend.Calls().size() == 2);
    CHECK(backend.Calls()[0].prompt.messages.back().content == "Fold for the scope.");
    CHECK(backend.Calls()[1].prompt.messages.back().content == "Condense these 3 for the scope.");
    CHECK(harness.store.Summaries().empty());
}
```

Update `Harness::Upkeep` to pass a run-wide scope like `Session`'s (Step 4), keeping `folded` recorded:

```cpp
    turn::MemoryUpkeep Upkeep(test::ScriptedBackend& backend, const core::Settings& settings)
    {
        turn::SummaryScope scope;
        scope.shown = [this] { return store.Summaries(); };
        scope.storeFold = [this](const core::Summary& summary) {
            store.AddSummary(summary);
            folded.push_back(summary);
        };
        scope.storeCondensed = [this](const core::Summary& condensed) { store.ReplaceSummaries(condensed); };
        scope.foldInstruction = std::string(turn::SUMMARIZE_INSTRUCTION);
        scope.condenseMessage = [](const std::vector<core::Summary>&) { return std::string(turn::CONDENSE_INSTRUCTION); };
        return turn::MemoryUpkeep(store, backend, settings, "the test tail", 9, 0, std::move(scope));
    }
```

Every other test in the file stays as it is.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`'SummaryScope': is not a member of 'morpheus::turn'`).

- [ ] **Step 3: Folding takes its instructions**

In `src/turn/Folding.h/.cpp`: `SummarizePrompt`, `SummarizeTurns`, `CondensePrompt` and `CondenseSummaries` take the instruction (or the condense message) as a `std::string_view` parameter placed as in **Produces**, and use it where they used `SUMMARIZE_INSTRUCTION` / `CONDENSE_INSTRUCTION`. Update their comments ("then `instruction`"). Update every caller (`grep -rn "SummarizePrompt\|SummarizeTurns\|CondensePrompt\|CondenseSummaries" src tests`).

- [ ] **Step 4: The upkeep works through the scope**

In `MemoryUpkeep`: the member `StoreFold _storeFold` becomes `SummaryScope _scope`; the constructor requires every function of the scope and both texts (`MORPHEUS_REQUIRE`, "MemoryUpkeep: a scope needs its summaries, its stores and its instructions"). `FoldOldest` calls `SummarizePrompt(systemMessage, oldest(count), _scope.foldInstruction)` and `SummarizeTurns(..., _scope.foldInstruction, ...)`, then `_scope.storeFold(*summary)`. `Condense` reads `const std::vector<core::Summary> summaries = _scope.shown();`, builds `const std::string message = _scope.condenseMessage(summaries);`, measures `CondensePrompt(systemMessage, message)`, calls `CondenseSummaries(systemMessage, summaries, message, ...)`, and stores with `_scope.storeCondensed(*merged)` instead of `_pStore->ReplaceSummaries`. `_pStore` stays for the run's seed (`Meta().rngSeed`).

In `src/turn/Session.cpp`, `Session::Upkeep` passes the run-wide scope:

```cpp
    // The run-wide scope: the owned run's summaries and the engine's texts, as before the scope existed.
    SummaryScope scope;
    scope.shown = [this] { return _store.Summaries(); };
    scope.storeFold = [this](const core::Summary& summary) {
        _store.AddSummary(summary);
        _index.SyncHistory(_store, summary.toTurn, *_pBackend);
    };
    scope.storeCondensed = [this](const core::Summary& condensed) { _store.ReplaceSummaries(condensed); };
    scope.foldInstruction = std::string(SUMMARIZE_INSTRUCTION);
    scope.condenseMessage = [](const std::vector<core::Summary>&) { return std::string(CONDENSE_INSTRUCTION); };
    return MemoryUpkeep(_store, *_pBackend, _settings, "the input and the state", turn, attempt, std::move(scope));
```

In `src/turn/HostedSession.cpp`, `Upkeep` passes the speaker's scope with the same texts (Task 4 gives hosted runs their own texts): `shown` reads `_store.ActorSummaries(speakerId, false)`, `storeFold` is Task 1's fold (with the actor and conversation set, then `SyncHistory()`), `storeCondensed` sets `actorId` and calls `_store.CondenseActorSummaries(speakerId, condensed)`.

- [ ] **Step 5: `LayOut` knows where the summaries sit**

In `src/context/PromptBuilder.h`:

```cpp
// Where a prompt's summaries sit: in the system message (the owned game's L2) or in the tail (a hosted speaker's
// memories). LayOut counts an over-cap summary at most at its cap where it sits, so condensing, not a settings error,
// answers it.
enum class SummaryPlace { System, Tail };
```

`LayOut` takes `SummaryPlace place` after `budget`. With `System`, it computes exactly what it computes today. With `Tail`:

```cpp
    // A hosted speaker's memories sit in the tail: an over-cap summary is counted at most at its cap there, and the
    // system message holds none.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.tailOverBudget = (place == SummaryPlace::Tail ? tokens.tail - summaryExcess : tokens.tail) >
                              budget.tailAllowance;
    const int systemShare = place == SummaryPlace::System ? systemCost - summaryExcess : systemCost;
```

and use `systemShare` where the transcript budget and its error used `systemCost - summaryExcess`. `AssembleResolvePrompt` passes `SummaryPlace::System`; `AssembleHostedPrompt` passes `SummaryPlace::System` for now (Task 4 moves its summaries).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[upkeep]"`, `"[turn]"`, `"[replay]"`, `"[context]"`, `"[hosted]"`.
Expected: `All tests passed` for each, with no existing `Session` test or replay edited.

- [ ] **Step 7: Commit**

```bash
git add src/turn/Folding.h src/turn/Folding.cpp src/turn/MemoryUpkeep.h src/turn/MemoryUpkeep.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Session.cpp src/turn/HostedSession.cpp tests/turn/MemoryUpkeepTests.cpp
git commit -m "SummaryScope: the memory upkeep reads and writes summaries through a scope; the owned game passes the run-wide one"
```

---

