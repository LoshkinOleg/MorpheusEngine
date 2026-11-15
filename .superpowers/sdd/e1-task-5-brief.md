### Task 5: Every NPC catches up: before it speaks, and in the background; R6's three-actor fixture

**Files:**
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Create: `tests/hosted/fixtures/three_actors.json`
- Modify: `tests/hosted/fixtures/README.md`, `tests/hosted/HostedTestSupport.h`
- Test: `tests/hosted/HostedMemoryTests.cpp`, `tests/hosted/HostedFixtureTests.cpp`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces:
  - `HostedSession` private: `bool CatchUp(const std::string& conversationId, const std::string& actorId, int throughTurn, int seedTurn, std::stop_token stop)` (folds `actorId`'s witnessed turns of the conversation after its mark through `throughTurn` into its memory, then sets its mark to `throughTurn`; false when cancelled) and `std::vector<std::string> NpcWitnesses(const std::vector<core::HostedTurn>& turns)` (every NPC who spoke or listened, in first-seen order).
  - `RunTurn` catches the speaker up before it speaks; `Maintain` catches up every NPC, folds ended conversations for every NPC witness, and leaves condensing to the upkeep.
  - In `tests/hosted/HostedTestSupport.h`: `test::BELETHOR` (`"0001B07D"`), `test::MemoryEchoResponder(HostedScript& script)` and `test::PlayMemoryFixture(const std::string& name, ...)` (the runner Task 6 and 7 extend).

- [ ] **Step 1: The fixture and its format**

Create `tests/hosted/fixtures/three_actors.json`:

```json
{
  "description": "R6's acceptance: Hulda, Saadia and Belethor with Aela, the player. Belethor leaves halfway; Hulda tells Aela a secret while Saadia is away. Nobody may remember what they did not witness.",
  "observation": {
    "game_time": {"days_passed": 213.42, "hour": 14},
    "location": "Whiterun, The Bannered Mare",
    "actors": [
      {"id": "0001A6A4", "name": "Hulda", "is_player": false, "bio": "Owner of the Bannered Mare."},
      {"id": "0001B07C", "name": "Saadia", "is_player": false, "bio": "A Redguard working at the inn."},
      {"id": "0001B07D", "name": "Belethor", "is_player": false, "bio": "A Breton trader."},
      {"id": "00000007", "name": "Aela", "is_player": true}
    ]
  },
  "participants": ["0001A6A4", "0001B07C", "0001B07D", "00000007"],
  "steps": [
    {"turn": {"speaker": "0001A6A4", "player_text": "Good evening, everyone."},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Welcome to the Bannered Mare, all of you."}},
    {"turn": {"speaker": "0001B07D", "player_text": "What do you sell?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "My wares are the finest in Whiterun."}},
    {"leave": ["0001B07D"]},
    {"turn": {"speaker": "0001A6A4", "player_text": "Where did Belethor go?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Belethor went back to his shop."}},
    {"leave": ["0001B07C"]},
    {"turn": {"speaker": "0001A6A4", "player_text": "Tell me a secret."},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "The guard captain takes bribes from the Thieves Guild."}},
    {"join": ["0001B07C"]},
    {"turn": {"speaker": "0001B07C", "player_text": "Welcome back, Saadia."},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Did I miss anything?"},
     "expect": {"prompt_absent": ["takes bribes"]}},
    {"end": true},
    {"maintain": true},
    {"memories": {"actor": "0001B07D", "contains": ["finest in Whiterun"], "absent": ["back to his shop", "takes bribes"]}},
    {"memories": {"actor": "0001B07C", "contains": ["back to his shop"], "absent": ["takes bribes"]}},
    {"memories": {"actor": "0001A6A4", "contains": ["takes bribes", "Did I miss anything"]}},
    {"memories": {"actor": "00000007", "count": 0}},
    {"start": ["0001B07D", "0001B07C", "00000007"]},
    {"turn": {"speaker": "0001B07D", "player_text": "What do you know about the guard captain?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Only that he buys nothing from me."},
     "expect": {"prompt_absent": ["takes bribes", "back to his shop"]}},
    {"turn": {"speaker": "0001B07C", "player_text": "And you, Saadia?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "I know nothing of him."},
     "expect": {"prompt_absent": ["takes bribes"]}}
  ]
}
```

Document the format in `tests/hosted/fixtures/README.md` (a new section, "Memory fixtures (`three_actors.json`)"): the top-level `observation` (used by every request without its own), `participants`, and the step kinds: a turn step as in conversation fixtures (with optional `expect.prompt_absent` and `expect.prompt_contains`: substrings checked against every message of the turn's resolve prompt); `{"leave": [ids]}` and `{"join": [ids]}` (participants changes, with the fixture's observation); `{"end": true}`; `{"maintain": true}` (background upkeep until nothing is left: `Maintain` until it returns true); `{"memories": {"actor", "contains"?, "absent"?, "count"?}}` (substrings of the actor's memories, every summary's text, and how many it has); `{"start": [ids]}` (a new conversation of those participants, which later steps use). A fixture's model replies are used in step order; memory calls are answered by the echo responder (below), which is part of the test, not the format.

- [ ] **Step 2: The test support**

In `tests/hosted/HostedTestSupport.h`, add `inline const std::string BELETHOR = "0001B07D";` and:

```cpp
// A scripted model for memory tests: resolve and speak calls as HostedScript answers them, and memory calls echo what
// they read, so a memory holds exactly the lines its NPC witnessed. A fold call (HOSTED_SUMMARIZE_INSTRUCTION) answers
// the assistant messages of the turns it folds, joined by " "; a condense call (HOSTED_CONDENSE_INSTRUCTION) answers
// its memory lines ("- ..." in its last message, without the "- " and any "(told by X) "), joined by " ".
inline ScriptedBackend::Responder MemoryEchoResponder(HostedScript& script)
{
    return [&script](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        const std::string& last = prompt.messages.back().content;
        if (last.find("Summarize the turns above as") != std::string::npos) {
            std::string echoed;
            for (std::size_t k = 1; k + 1 < prompt.messages.size(); ++k) {
                if (prompt.messages[k].role == inference::Role::Assistant) {
                    echoed += (echoed.empty() ? "" : " ") + prompt.messages[k].content;
                }
            }
            return echoed;
        }
        if (last.find("Condense what") != std::string::npos) {
            std::string echoed;
            std::size_t at = 0;
            while ((at = last.find("\n- ", at)) != std::string::npos) {
                const std::size_t end = last.find('\n', at + 1);
                std::string line = last.substr(at + 3, end - (at + 3));
                if (line.starts_with("(told by ")) {
                    line = line.substr(line.find(") ") + 2);
                }
                echoed += (echoed.empty() ? "" : " ") + line;
                at = end;
            }
            return echoed;
        }
        return script.Responder()(prompt, spec);
    };
}
```

(The first memory line follows the heading line, so its "- " is preceded by "\n": RenderMemories' format guarantees it.)

Add the fixture runner, a free function the tests share:

```cpp
// Plays memory fixture `name` (format: fixtures/README.md, "Memory fixtures") on a fresh run with the echo responder,
// checking each step's expectations. Task 6 adds the share step.
inline void PlayMemoryFixture(const std::string& name)
{
    const nlohmann::json fixture = ReadFixture(name);
    const nlohmann::json& observation = fixture.at("observation");
    HostedScript script;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    ScriptedBackend backend(16384, MemoryEchoResponder(script));
    turn::HostedSession session =
        turn::HostedSession::Open(HostedRunFile("hosted_memory_fixture_" + name), FixtureHostData(), HostedSettings(), 42,
                                  backend);
    std::string id = session.StartConversation(fixture.at("participants").get<std::vector<std::string>>(), observation);

    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO(name << " step " << i);
        if (step.contains("turn")) {
            const nlohmann::json& turnJson = step.at("turn");
            turn::HostedTurnRequest request;
            request.conversationId = id;
            request.speakerId = turnJson.at("speaker").get<std::string>();
            if (turnJson.contains("player_text")) {
                request.playerText = turnJson.at("player_text").get<std::string>();
            }
            request.observation = observation;
            REQUIRE(session.RunTurn(request, {}, {}));
            // Every message of the turn's resolve prompt, for the expectations.
            std::string prompt;
            for (const inference::Message& message : session.LastAssembly().prompt.messages) {
                prompt += message.content + "\n";
            }
            const nlohmann::json expect = step.value("expect", nlohmann::json::object());
            for (const nlohmann::json& absent : expect.value("prompt_absent", nlohmann::json::array())) {
                CHECK_THAT(prompt, !Catch::Matchers::ContainsSubstring(absent.get<std::string>()));
            }
            for (const nlohmann::json& present : expect.value("prompt_contains", nlohmann::json::array())) {
                CHECK_THAT(prompt, Catch::Matchers::ContainsSubstring(present.get<std::string>()));
            }
        } else if (step.contains("leave")) {
            session.SetParticipants(id, {}, step.at("leave").get<std::vector<std::string>>(), observation);
        } else if (step.contains("join")) {
            session.SetParticipants(id, step.at("join").get<std::vector<std::string>>(), {}, observation);
        } else if (step.contains("end")) {
            session.EndConversation(id);
        } else if (step.contains("maintain")) {
            REQUIRE(session.Maintain({}));
        } else if (step.contains("memories")) {
            const nlohmann::json& check = step.at("memories");
            std::string texts;
            const std::vector<core::Summary> memories = session.Memories(check.at("actor").get<std::string>());
            for (const core::Summary& memory : memories) {
                texts += memory.text + "\n";
            }
            for (const nlohmann::json& present : check.value("contains", nlohmann::json::array())) {
                CHECK_THAT(texts, Catch::Matchers::ContainsSubstring(present.get<std::string>()));
            }
            for (const nlohmann::json& absent : check.value("absent", nlohmann::json::array())) {
                CHECK_THAT(texts, !Catch::Matchers::ContainsSubstring(absent.get<std::string>()));
            }
            if (check.contains("count")) {
                CHECK(memories.size() == check.at("count").get<std::size_t>());
            }
        } else if (step.contains("start")) {
            id = session.StartConversation(step.at("start").get<std::vector<std::string>>(), observation);
        } else {
            FAIL("unknown step kind");
        }
    }
}
```

(Add the includes this needs: `<catch2/catch_test_macros.hpp>`, `<catch2/matchers/catch_matchers_string.hpp>`, `"core/TurnRecord.h"`.)

`FixtureHostData()`'s observation schema must accept the fixture's observation (it has the same fields as observation.json); if the host schema refuses a field, adjust the fixture, not the schema.

- [ ] **Step 3: Write the failing tests**

Add to `tests/hosted/HostedFixtureTests.cpp`:

```cpp
TEST_CASE("The three-actor fixture: nobody remembers what it did not witness", "[hosted][fixtures][memory]")
{
    test::PlayMemoryFixture("three_actors.json");
}
```

Add to `tests/hosted/HostedMemoryTests.cpp`:

```cpp
TEST_CASE("An NPC behind in a conversation is caught up before it speaks", "[hosted][memory]")
{
    // A fold during Hulda's turn leaves Saadia behind; her next turn writes her memory of those turns first.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {std::string(3000, 'a'), std::string(3000, 'b'), "Short.", "Saadia here."};
    test::ScriptedBackend backend(4096, test::MemoryEchoResponder(script));
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 4096;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_catchup"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Three."), {}, {}));
    const int folded = session.Conversation(id)->foldedThrough;
    REQUIRE(folded >= 1);
    CHECK(session.Memories(test::SAADIA).empty());

    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "Saadia?"), {}, {}));
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE_FALSE(saadia.empty());
    CHECK(saadia.back().toTurn <= folded);
    CHECK(saadia[0].text.find("aaaa") != std::string::npos); // She witnessed the first turn.
}
```

(Tune the sizes as in Task 4's fold test.)

- [ ] **Step 4: Run them to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[memory]"`
Expected: the fixture test FAILS (Belethor and Saadia have no memories after `maintain`: only the last speaker's is written), and the catch-up test FAILS (Saadia has none).

- [ ] **Step 5: Catch-up and the background upkeep**

In `src/turn/HostedSession.cpp`:

```cpp
std::vector<std::string> HostedSession::NpcWitnesses(const std::vector<core::HostedTurn>& turns)
{
    // Who spoke or listened, as an NPC: a turn's player is in its observation.
    std::vector<std::string> npcs;
    auto add = [&npcs](const std::string& id) {
        if (std::find(npcs.begin(), npcs.end(), id) == npcs.end()) {
            npcs.push_back(id);
        }
    };
    for (const core::HostedTurn& turn : turns) {
        const std::vector<core::Actor> actors = HostedWorld::ActorsOf(turn.observation);
        auto isPlayer = [&actors](const std::string& id) {
            return std::any_of(actors.begin(), actors.end(),
                               [&id](const core::Actor& actor) { return actor.id == id && actor.isPlayer; });
        };
        add(turn.speakerId);
        for (const std::string& listener : turn.listenerIds) {
            if (!isPlayer(listener)) {
                add(listener);
            }
        }
    }
    return npcs;
}

bool HostedSession::CatchUp(const std::string& conversationId, const std::string& actorId, int throughTurn,
                            int seedTurn, std::stop_token stop)
{
    // Nothing to do when the actor's memory already reaches throughTurn.
    if (_store.RememberedThrough(conversationId, actorId) >= throughTurn) {
        return true;
    }
    // The actor's witnessed turns after its mark through throughTurn fold into its memory (FoldAll: in one call when
    // they fit), each pass reading the mark again: a fold moves it.
    const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversationId);
    const nlohmann::json observation = turns.empty() ? nlohmann::json::object() : turns.back().observation;
    auto assemble = [&](const std::vector<RetrievalHit>&) {
        const core::Conversation now = *_store.Conversation(conversationId);
        context::HostedInputs inputs;
        inputs.observation = observation;
        inputs.instruction = std::string(MAINTENANCE_INSTRUCTION);
        return Pass(conversationId, actorId,
                    Entries(now, _store.RememberedThrough(conversationId, actorId), throughTurn, actorId), {}, inputs);
    };
    if (!Upkeep(conversationId, actorId, seedTurn, false).FoldAll(assemble, stop)) {
        return false;
    }
    // Turns it did not witness need no memory: its mark reaches throughTurn.
    if (_store.RememberedThrough(conversationId, actorId) < throughTurn) {
        _store.SetRememberedThrough(conversationId, actorId, throughTurn);
    }
    return true;
}
```

`RunTurn`, right after the request's checks and before retrieval:

```cpp
    // The speaker's own memory of this conversation reaches the transcript's cut before it speaks, so nothing it
    // witnessed is in neither its L3 nor its memory.
    if (!CatchUp(conversation.id, request.speakerId, conversation.foldedThrough, turn, stop)) {
        return std::nullopt;
    }
```

`Maintain`:

```cpp
bool HostedSession::Maintain(std::stop_token stop)
{
    // Every NPC catches up in every conversation: an ended conversation is folded through its last turn first, for
    // every NPC witness; an open one through its current cut. Condensing happens in the upkeep, per NPC, when an NPC's
    // memories pass summary_cap. Seeds name the run's latest turn.
    const int latest = _store.LatestHostedTurn();
    for (const core::Conversation& conversation : _store.Conversations()) {
        const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversation.id);
        if (turns.empty()) {
            continue;
        }
        int through = conversation.foldedThrough;
        if (conversation.ended && turns.back().number > through) {
            through = turns.back().number;
            _store.MarkFolded(conversation.id, through);
            SyncHistory();
        }
        for (const std::string& npc : NpcWitnesses(turns)) {
            if (!CatchUp(conversation.id, npc, through, latest, stop)) {
                return false;
            }
        }
    }
    return true;
}
```

Remove Task 4's interim "last speaker" folding from `Maintain`. Declare `CatchUp` and `NpcWitnesses` in the header with comments (both have several callers: `CatchUp` in `RunTurn` and `Maintain`, `NpcWitnesses` in `Maintain` and Task 6's `ShareMemories`).

Update D1's tests of `Maintain` (`tests/hosted/HostedFoldingTests.cpp`): an ended conversation is now folded for every NPC witness; check each NPC's memories where they checked the run-wide summaries.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[memory]"`, `"[hosted]"`, `"[turn]"`, `"[replay]"`, `"[hosted-server]"`.
Expected: `All tests passed` for each.

- [ ] **Step 7: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/
git commit -m "Every NPC catches up on what it witnessed: before it speaks and in the background; R6's three-actor fixture"
```

---

