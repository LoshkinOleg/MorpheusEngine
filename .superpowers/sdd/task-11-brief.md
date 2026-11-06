### Task 11: Assisted memory mode (lookups and notes)

**Files:**
- Modify: `src/turn/Session.cpp`, `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `BuildLookupGrammar`, `ParseLookupOutput`, `LOOKUP_INSTRUCTION`, `LOOKUP_MAX_HITS`, `LOOKUP_MAX_TOKENS`, `MemoryIndex::Retrieve`.
- Produces: behavior only. In assisted mode, a turn makes a lookup call before resolving:
  - its hits join the tail after the automatic ones, so the fit loop sheds them first;
  - `timings["lookup"]` records the call, its requests and the hits each added;
  - resolve notes are committed with the turn and become retrievable after `Maintain`.

- [ ] **Step 1: Write the failing test**

Append to `tests/turn/SessionTests.cpp`:

```cpp
TEST_CASE("Assisted mode looks things up before resolving and keeps notes", "[session]")
{
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    const std::string listen =
        R"({"intent":"listen","checks":[],"changes":[],"notes":["The keeper hums an old sea shanty."]})";
    test::ScriptedBackend backend(16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        if (spec.grammar.find("lookups") != std::string::npos) {
            return std::string(R"({"lookups":[{"source":"lore","query":"keeper"}]})");
        }
        return Responder(listen, "The keeper hums.")(prompt, spec);
    });
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("session_assisted"), 42, backend);

    const std::optional<core::TurnRecord> record = session.RunTurn("I listen.", {}, {});
    REQUIRE(record);
    const auto& calls = backend.Calls(); // lookup, resolve, narrate
    REQUIRE(calls.size() == 3);
    CHECK_THAT(calls[0].prompt.messages.back().content, ContainsSubstring(std::string(turn::LOOKUP_INSTRUCTION)));
    CHECK_THAT(calls[1].prompt.messages.back().content, ContainsSubstring("keeper: The keeper says"));
    CHECK_THAT(calls[1].prompt.messages.front().content, ContainsSubstring("# Memory"));
    CHECK_THAT(calls[1].spec.grammar, ContainsSubstring("notes ::="));
    CHECK(record->notes == std::vector<std::string>{"The keeper hums an old sea shanty."});
    REQUIRE(record->timings.contains("lookup"));
    CHECK(record->timings.at("lookup").at("requests").at(0).at("added") == nlohmann::json::array({"keeper"}));

    // After maintenance the note is searchable memory for later turns.
    REQUIRE(session.Maintain({}));
    REQUIRE(session.RunTurn("What shanty does the keeper hum?", {}, {}));
    CHECK_THAT(backend.Calls().at(4).prompt.messages.back().content,
               ContainsSubstring("Note from turn 1: The keeper hums an old sea shanty."));
}

TEST_CASE("Engine mode makes no lookup call and keeps no notes", "[session]")
{
    test::ScriptedBackend backend(16384, Responder(R"({"intent":"listen","checks":[],"changes":[]})"));
    turn::Session session = turn::Session::Start(MiniWorld(), TestSettings(), NewRunFile("session_engine_mode"), 42, backend);
    REQUIRE(session.RunTurn("I listen.", {}, {}));
    CHECK(backend.Calls().size() == 2);
    CHECK_THAT(backend.Calls().at(0).prompt.messages.front().content, !ContainsSubstring("# Memory"));
}
```

The second turn's calls are lookup (3), resolve (4) and narrate (5), so index 4 is its resolve call.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session]"`
Expected: FAIL. The assisted test sees 2 calls, not 3.

- [ ] **Step 3: Implement**

In `Session::PlayTurn`, between "1. Retrieve" and "2. Resolve":

```cpp
    // Assisted mode: the model may ask for lookups first. Their hits join the tail after the automatic ones, so
    // the fit loop sheds them first.
    if (_mode == core::MemoryMode::Assisted) {
        std::optional<context::Assembly> lookupAssembly =
            AssembleFitting(state, hits, input, LOOKUP_INSTRUCTION, turn, attempt, stop);
        if (!lookupAssembly) {
            return std::nullopt;
        }
        inference::GenerationSpec lookupSpec;
        lookupSpec.grammar = BuildLookupGrammar();
        lookupSpec.maxTokens = LOOKUP_MAX_TOKENS;
        lookupSpec.sampling = inference::SamplingFrom(
            _settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Lookup, attempt));
        const inference::Completion looked = _pBackend->Complete(lookupAssembly->prompt, lookupSpec, {}, stop);
        if (looked.cancelled) {
            return std::nullopt;
        }

        std::string lookupError;
        nlohmann::json lookupTelemetry = Telemetry(looked);
        lookupTelemetry["requests"] = nlohmann::json::array();
        for (const Lookup& lookup : ParseLookupOutput(looked.text, lookupError)) {
            const std::array<store::EmbeddingKind, 1> source = {lookup.source};
            nlohmann::json added = nlohmann::json::array();
            for (RetrievalHit& hit :
                 _index.Retrieve(lookup.query, source, _settings.budget.retrievalCap, LOOKUP_MAX_HITS, *_pBackend)) {
                const bool known = std::any_of(hits.begin(), hits.end(), [&hit](const RetrievalHit& other) {
                    return other.kind == hit.kind && other.refId == hit.refId;
                });
                if (!known) {
                    added.push_back(hit.refId);
                    hits.push_back(std::move(hit));
                }
            }
            lookupTelemetry["requests"].push_back(
                {{"source", kindText(lookup.source)}, {"query", lookup.query}, {"added", std::move(added)}});
        }
        if (!lookupError.empty()) {
            lookupTelemetry["error"] = lookupError;
        }
        telemetry["lookup"] = std::move(lookupTelemetry);
    }
```

The `retrieval` telemetry written at commit lists the hits that survived the fit loop, so lookups that did not fit are visible as requests whose `added` refs are missing there.

In the "Mini world" automatic retrieval for "I listen." nothing matches, so `keeper` is added by the lookup. If the bag-of-words embedding happens to retrieve `keeper` automatically, `added` would be empty. In that case change the test's turn input to one that shares no words with the lore, and report it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn/Session.cpp tests/turn/SessionTests.cpp
git commit -m "Add assisted memory mode: lookups before resolve, notes with the turn" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

