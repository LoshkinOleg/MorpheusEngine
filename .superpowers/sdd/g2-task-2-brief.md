### Task 2: The call tag

**Files:**
- Modify: `src/inference/Llm.h` (`GenerationSpec`), `src/turn/Session.cpp` (lookup, resolve, narrate/renarrate, review specs), `src/turn/HostedSession.cpp` (decide, resolve, speak specs), `src/turn/MemoryUpkeep.cpp` (fold, condense specs), `src/turn/Replay.h`, `src/turn/Replay.cpp` (`CallKindOf`, `RecordingBackend::Complete`), `src/app/model/ModelSeat.h`, `src/app/model/ModelSeat.cpp` (`SeatLabelOf`), `src/app/model/ModelSeatBackend.cpp` (its call), `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/grammar/Gbnf.h` (comments only)
- Test: `tests/turn/ReplayTests.cpp`, `tests/app/ModelSeatTests.cpp`, `tests/turn/CallTagTests.cpp` (new), `tests/hosted/HostedDecideTests.cpp`

**Interfaces:**
- Produces: `inference::GenerationSpec::callTag` (`std::string`, default empty); `core::CallKind turn::CallKindOf(const inference::GenerationSpec& spec)`; `app::SeatLabel app::SeatLabelOf(const inference::GenerationSpec& spec)`.

- [ ] **Step 1: Write the failing tests**

`tests/turn/ReplayTests.cpp`: the replay responder calls `turn::CallKindOf(spec)` (drop the prompt argument). Replace the test "CallKindOf tells the calls apart and CallKindName spells them" with:

```cpp
TEST_CASE("CallKindOf reads the call tag and CallKindName spells the kinds", "[replay]")
{
    auto kindOf = [](const std::string& tag) {
        inference::GenerationSpec spec;
        spec.callTag = tag;
        return turn::CallKindOf(spec);
    };
    CHECK(kindOf("lookup") == core::CallKind::Lookup);
    CHECK(kindOf("resolve") == core::CallKind::Resolve);
    CHECK(kindOf("narrate") == core::CallKind::Narrate);
    CHECK(kindOf("renarrate") == core::CallKind::Narrate); // Its replies join the narrate replies.
    CHECK(kindOf("review") == core::CallKind::Review);
    CHECK(kindOf("summarize") == core::CallKind::Summarize);
    CHECK(kindOf("condense") == core::CallKind::Summarize);
    // The replay and the app see only the owned game's calls: anything else is an engine bug.
    CHECK_THROWS_WITH(kindOf(""), "CallKindOf: no call kind for the call tag ''");
    CHECK_THROWS_WITH(kindOf("speak"), "CallKindOf: no call kind for the call tag 'speak'");
    CHECK_THROWS_WITH(kindOf("decide"), "CallKindOf: no call kind for the call tag 'decide'");

    // [keep the existing CallKindName checks of the old test here, unchanged]
}
```

(The bracketed line is plan shorthand: move the old test's `CallKindName` CHECKs into this one verbatim.)

`tests/app/ModelSeatTests.cpp`: replace "SeatLabelOf labels each call the way the session and folding make it" with:

```cpp
TEST_CASE("SeatLabelOf labels each call by its tag", "[model_seat]")
{
    auto labelOf = [](const std::string& tag) {
        inference::GenerationSpec spec;
        spec.callTag = tag;
        return app::SeatLabelOf(spec);
    };
    CHECK(labelOf("resolve") == app::SeatLabel::Resolve);
    CHECK(labelOf("narrate") == app::SeatLabel::Narrate);
    CHECK(labelOf("renarrate") == app::SeatLabel::Rewrite);
    CHECK(labelOf("lookup") == app::SeatLabel::Lookup);
    CHECK(labelOf("review") == app::SeatLabel::Review);
    CHECK(labelOf("summarize") == app::SeatLabel::Summarize);
    CHECK(labelOf("condense") == app::SeatLabel::Condense);
    CHECK_THROWS_WITH(labelOf(""), "SeatLabelOf: no seat label for the call tag ''");
    CHECK_THROWS_WITH(labelOf("speak"), "SeatLabelOf: no seat label for the call tag 'speak'");
}
```

Remove the includes that test no longer needs. Any other `SeatLabelOf(prompt, spec)` call in `tests/app/` becomes `SeatLabelOf(spec)` with a tagged spec.

Create `tests/turn/CallTagTests.cpp` (owned game): run turns of the mini world with a `test::ScriptedBackend` whose responder answers by `spec.callTag` and records every tag, with `narration_check` on so a review runs, and a review answer that reports a contradiction once so a renarrate runs; in assisted memory mode so a lookup runs; and settings with a small transcript budget (or enough turns) so a fold runs. Check the recorded tags contain each of `lookup`, `resolve`, `narrate`, `review`, `renarrate`, `summarize`, and that every call's tag is one of the owned tags. Model the setup on the existing `tests/turn/SessionTests.cpp` cases that make a review, a renarrate and a lookup (grep `RENARRATE_CORRECTION` and `Assisted` there), and on `tests/turn/FoldingTests.cpp` for a fold and a condense; reuse their settings and world setup rather than inventing new ones. If one test cannot reach a call kind cheaply, split it into two cases; every one of the seven owned tags (`condense` included) must be checked somewhere in this file.

`tests/hosted/HostedDecideTests.cpp`, the first case ("Decide chooses among the NPC participants ..."): add `CHECK(calls[0].spec.callTag == "decide"); CHECK(calls[1].spec.callTag == "resolve"); CHECK(calls[2].spec.callTag == "speak");`. In `tests/hosted/HostedPromptPackTests.cpp`'s first case, add `CHECK(backend.Calls().back().spec.callTag == "summarize");` after the fold, and in its condense case `CHECK(call.spec.callTag == "condense");` inside the condense branch.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`callTag` is not a member; `CallKindOf` and `SeatLabelOf` take two arguments).

- [ ] **Step 3: Implement**

`src/inference/Llm.h`, `GenerationSpec`, last member:

```cpp
    // What the call is, for the layers that tell calls apart (the replay, the app's labels); the backends ignore it.
    // The turn layer sets it on every call (generation-controls spec, section 2).
    std::string callTag = {};
```

Set the tag where each spec is built:
- `src/turn/Session.cpp`: `lookupSpec.callTag = "lookup";`, `resolveSpec.callTag = "resolve";`, `reviewSpec.callTag = "review";`; in the `narrate` lambda, `spec.callTag = kind == core::CallKind::Renarrate ? "renarrate" : "narrate";`.
- `src/turn/HostedSession.cpp`: `decideSpec.callTag = "decide";`, `resolveSpec.callTag = "resolve";`, `speakSpec.callTag = "speak";`.
- `src/turn/MemoryUpkeep.cpp`: the fold's `spec.callTag = "summarize";`, the condense's `spec.callTag = "condense";`.

`src/turn/Replay.h`: `core::CallKind CallKindOf(const inference::GenerationSpec& spec);` with the comment: "Which call a completion answers, read from its tag: lookup, resolve, narrate, review, summarize; a renarrate counts as narration and a condense as a summary (their replies join those kinds', in call order). Throws ContractError for any other tag: the replay and the app see only the owned game's calls."

`src/turn/Replay.cpp`, `CallKindOf`:

```cpp
core::CallKind CallKindOf(const inference::GenerationSpec& spec)
{
    // The tag the turn layer set; a renarrate's and a condense's replies join the narrate and summarize replies.
    const std::string& tag = spec.callTag;
    if (tag == "lookup") {
        return core::CallKind::Lookup;
    }
    if (tag == "resolve") {
        return core::CallKind::Resolve;
    }
    if (tag == "narrate" || tag == "renarrate") {
        return core::CallKind::Narrate;
    }
    if (tag == "review") {
        return core::CallKind::Review;
    }
    if (tag == "summarize" || tag == "condense") {
        return core::CallKind::Summarize;
    }
    throw core::ContractError(std::format("CallKindOf: no call kind for the call tag '{}'", tag));
}
```

(drop the `EngineText.h` and `ResolveGrammar.h` includes if nothing else in the file needs them; add `<format>`). `RecordingBackend::Complete` calls `CallKindOf(spec)`.

`src/app/model/ModelSeat.h`: `SeatLabel SeatLabelOf(const inference::GenerationSpec& spec);` with the comment "The label of a call, from its tag (the turn layer's): resolve, narrate, renarrate (a rewrite), lookup, review, summarize, condense. Throws ContractError for any other tag." `src/app/model/ModelSeat.cpp`:

```cpp
SeatLabel SeatLabelOf(const inference::GenerationSpec& spec)
{
    // The tag the turn layer set: each owned call has its own label.
    const std::string& tag = spec.callTag;
    if (tag == "resolve") {
        return SeatLabel::Resolve;
    }
    if (tag == "narrate") {
        return SeatLabel::Narrate;
    }
    if (tag == "renarrate") {
        return SeatLabel::Rewrite;
    }
    if (tag == "lookup") {
        return SeatLabel::Lookup;
    }
    if (tag == "review") {
        return SeatLabel::Review;
    }
    if (tag == "summarize") {
        return SeatLabel::Summarize;
    }
    if (tag == "condense") {
        return SeatLabel::Condense;
    }
    throw core::ContractError(std::format("SeatLabelOf: no seat label for the call tag '{}'", tag));
}
```

(drop includes it no longer needs). `src/app/model/ModelSeatBackend.cpp`: `SeatLabelOf(spec)`.

Comments: `src/turn/ResolveGrammar.h` (the lookup grammar's "Its first line, the root, is how turn::CallKindOf tells a lookup call"), `src/turn/ResolveGrammar.cpp` (lines ~178 and ~211) and `src/grammar/Gbnf.h` (~72): drop the clauses that say the root line exists for `CallKindOf`; the grammars themselves do not change.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`, then `ctest --preset tests`
Expected: all pass, the replay of every recorded fixture included (it now reads every owned call's tag: a missing tag would be a ContractError there).

- [ ] **Step 5: Commit**

```bash
git add src/inference/Llm.h src/turn/Session.cpp src/turn/HostedSession.cpp src/turn/MemoryUpkeep.cpp src/turn/Replay.h src/turn/Replay.cpp src/app/model/ModelSeat.h src/app/model/ModelSeat.cpp src/app/model/ModelSeatBackend.cpp src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/grammar/Gbnf.h tests/turn/ReplayTests.cpp tests/app/ModelSeatTests.cpp tests/turn/CallTagTests.cpp tests/hosted/HostedDecideTests.cpp tests/hosted/HostedPromptPackTests.cpp
git commit -m "Call tags: every turn-layer call names itself; the replay and the app's labels read the tag"
```

---

