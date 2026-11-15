### Task 2: The event overflow cure: the newest events that fit stay in the prompt

**Files:**
- Modify: `src/turn/HostedSession.cpp` (`RunTurn`, `Entries`)
- Modify: `src/turn/HostedSession.h` (the comments of `RecordEvents` and `RunTurn`)
- Test: `tests/hosted/HostedTurnTests.cpp`

**Interfaces:**
- Consumes: `core::HostedTurn::eventsOmitted` (Task 1).
- Produces: `RunTurn` no longer throws `TurnError` for events alone: when the tail is over `tail_allowance` with no retrieved snippet left, the oldest events leave the prompt until it fits, and `record.eventsOmitted` counts them. `TurnError` remains when the observation, the player text and the instruction alone do not fit. A turn's L3 entry leaves out the same events.

- [ ] **Step 1: Write the failing tests**

In `tests/hosted/HostedTurnTests.cpp`, delete the whole test case `"Events recorded between turns over the tail allowance make every later turn a TurnError until the conversation ends"` and put these two in its place (they use the file's `OpenRun`, `NONE`, and `test::Request`, `test::HULDA`, `test::AELA`, `test::FixtureObservation`, `test::HostedSettings` from `hosted/HostedTestSupport.h`):

```cpp
TEST_CASE("Events over the tail allowance: the oldest leave the prompt, the newest stay, and the turn counts them",
          "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"Welcome.", "Hm.", "Yes?"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_events_overflow", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));

    // Four events of 300 tokens each (the fake counts 4 bytes a token): with the observation and the player's line,
    // over tail_allowance 1024. Three are recorded between turns, the fourth comes with the request.
    const std::string a(1200, 'a');
    const std::string b(1200, 'b');
    const std::string c(1200, 'c');
    const std::string d(1200, 'd');
    session.RecordEvents(id, {nlohmann::json{{"text", a}}, nlohmann::json{{"text", b}}});
    session.RecordEvents(id, {nlohmann::json{{"text", c}}});
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Still there?");
    request.events = {nlohmann::json{{"text", d}}};
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);

    // The oldest left the prompt; the newest and the player's line stayed, and the tail fits.
    const context::Assembly& assembly = session.LastAssembly();
    const std::string& tail = assembly.prompt.messages.back().content;
    CHECK(record->eventsOmitted >= 1);
    CHECK(record->eventsOmitted < 4);
    CHECK(assembly.tokens.tail <= test::HostedSettings().budget.tailAllowance);
    CHECK_THAT(tail, !ContainsSubstring(a));
    CHECK_THAT(tail, ContainsSubstring("(" + d + ")"));
    CHECK_THAT(tail, ContainsSubstring("Still there?"));
    // The store keeps every event, and the turn keeps its count.
    CHECK(session.Events(id).size() == 3);
    CHECK(session.Turns(id).back().events.size() == 1);
    CHECK(session.Turns(id).back().eventsOmitted == record->eventsOmitted);

    // The next turn's L3 shows that turn with the events its prompt showed: not the oldest, still the newest.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Well?"), {}, {}));
    const inference::Prompt& next = session.LastAssembly().prompt;
    bool showsOldest = false;
    bool showsNewest = false;
    for (std::size_t k = 1; k < next.stableCount; ++k) {
        showsOldest = showsOldest || next.messages[k].content.find(a) != std::string::npos;
        showsNewest = showsNewest || next.messages[k].content.find(d) != std::string::npos;
    }
    CHECK_FALSE(showsOldest);
    CHECK(showsNewest);
}

TEST_CASE("A tail over the allowance even without its events is still a TurnError, before any model call",
          "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_tail_over", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.RecordEvents(id, {nlohmann::json{{"text", "A door opens."}}});
    const turn::HostedTurnRequest request = test::Request(id, test::HULDA, std::string(5000, 'x'));
    CHECK_THROWS_MATCHES(session.RunTurn(request, {}, {}), turn::TurnError,
                         MessageMatches(ContainsSubstring("the observation, the events and the player text use")));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
    CHECK(session.Events(id).size() == 1);
}
```

Add `#include "context/PromptBuilder.h"` to the file's includes if `context::Assembly` is not already visible through `turn/HostedSession.h` (it is declared in `context/PromptBuilder.h`).

- [ ] **Step 2: Run the tests to see the first fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted][turn]"`
Expected: the first new test FAILS with a `TurnError` ("the observation, the events and the player text use ... over tail_allowance 1024"); the second passes already.

- [ ] **Step 3: Trim the oldest events in RunTurn**

In `src/turn/HostedSession.cpp`, `RunTurn`, replace the fit call:

```cpp
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(conversation.id, kept, tail); }, stop);
```

with:

```cpp
    // The fit loop sheds retrieved hits first. When the tail is still over tail_allowance with none left, the oldest
    // events leave the prompt one by one until it fits (the store keeps them all; the record counts them). The
    // observation, the player text and the instruction must fit on their own: with no event left, an over-long tail
    // is the fit loop's TurnError, before any model call.
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits,
        [&](const std::vector<RetrievalHit>& kept) {
            FitPass pass = Pass(conversation.id, kept, tail);
            while (pass.assembly.tailOverBudget && kept.empty() && !tail.events.empty()) {
                tail.events.erase(tail.events.begin());
                pass = Pass(conversation.id, kept, tail);
            }
            return pass;
        },
        stop);
```

In step 4 of `RunTurn` (the record), after `record.line = core::Trim(spoken.text);`:

```cpp
    record.eventsOmitted = static_cast<int>(newEvents.size() - tail.events.size());
```

- [ ] **Step 4: Leave the omitted events out of the turn's L3 entry**

In `HostedSession::Entries`, replace:

```cpp
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            if (turn.playerText) {
```

with:

```cpp
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            // The events its prompt left out (the oldest, when the tail was over tail_allowance) stay out of its
            // entry too, so one long burst of events does not overflow L3 later.
            before.erase(before.begin(),
                         before.begin() + std::min<std::ptrdiff_t>(turn.eventsOmitted, std::ssize(before)));
            if (turn.playerText) {
```

- [ ] **Step 5: Update the contract comments**

In `src/turn/HostedSession.h`, replace the comment of `RecordEvents` with:

```cpp
    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn: every event recorded since its last turn joins that turn's tail. When the tail is over
    // tail_allowance, the oldest of them are left out of that turn's prompt (RunTurn); the store keeps them all.
    // Throws DataError naming the field.
```

and in the comment of `RunTurn`, replace the sentences from "Throws DataError naming the field for a malformed request" to the end of the paragraph with:

```cpp
    // Throws DataError naming the field for a malformed request (before any model call), TurnError when the
    // observation, the player text and the instruction alone do not fit tail_allowance (before the resolve call), and
    // InferenceError from the backend. The tail holds the observation, every event recorded since the conversation's
    // last turn (RecordEvents), the request's events and the player text: when it is over tail_allowance with no
    // retrieved snippet left to shed, the oldest events leave the prompt one by one until it fits. The record's
    // eventsOmitted counts them; the store keeps them, and the turn's L3 entry leaves them out as its prompt did.
    // Model mistakes are not exceptions: a refused action is recorded with its reasons and the turn goes on with no
    // action.
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: `All tests passed`.

Then the owned game's tests, which share the fit loop: `./build/tests/bin/Debug/morpheus_tests.exe "[turn]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTurnTests.cpp
git commit -m "Hosted turns keep the newest events that fit: the oldest leave the prompt and its L3 entry, and the turn counts them"
```

---

