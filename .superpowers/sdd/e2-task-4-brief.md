### Task 4: `HostedSession::Rewind`, abandoning on new records, and the reload fixture

**Files:**
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Create: `tests/hosted/fixtures/reload.json`, `tests/hosted/HostedRewindTests.cpp`
- Modify: `tests/hosted/fixtures/README.md`, `tests/hosted/HostedTestSupport.h` (the fixture runner's new steps), `tests/hosted/HostedFixtureTests.cpp`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  - `struct turn::RewindResult { bool rewound = false; int hiddenTurns = 0; int restoredTurns = 0; std::optional<double> rollbackTime = {}; };`
  - `RewindResult HostedSession::Rewind(double gameTime)`.
  - Every writer of a new record (`StartConversation`, `RecordEvents`, `SetParticipants`, `EndConversation`, `RunTurn` once its request passed its checks, `ShareMemories` once it passed its checks) first abandons an open rollback (`_store.AbandonRollback()` when `_store.OpenRollback()`).
  - Memory fixture steps: `{"rewind": {"game_time", "rewound"?, "hidden_turns"?, "restored_turns"?}}`, `{"turns": {"count"}}` (the current conversation's visible turns), `{"conversation": {"ended"?, "participants"?, "events"?}}` (`events`: how many visible events), `{"events": [...]}` (`RecordEvents` on the current conversation), and a step-level `"clock"` on turn, leave, join and start steps (written into the observation at the fixture's clock pointer); the fixture's top-level `"clock"` names the pointer (added to `open.json`'s host data).

- [ ] **Step 1: The fixture**

Create `tests/hosted/fixtures/reload.json`:

```json
{
  "description": "R6's rollback acceptance: a conversation over several game times with events, a participant change, a share and an end. A rewind to the middle hides what came after and NPCs forget it; a rewind to the end restores it exactly; playing on after a rewind abandons the hidden future.",
  "clock": "/game_time/days_passed",
  "observation": {
    "game_time": {"days_passed": 1.0, "hour": 14},
    "location": "Whiterun, The Bannered Mare",
    "actors": [
      {"id": "0001A6A4", "name": "Hulda", "is_player": false, "bio": "Owner of the Bannered Mare."},
      {"id": "0001B07C", "name": "Saadia", "is_player": false, "bio": "A Redguard working at the inn."},
      {"id": "00000007", "name": "Aela", "is_player": true}
    ]
  },
  "participants": ["0001A6A4", "0001B07C", "00000007"],
  "steps": [
    {"turn": {"speaker": "0001A6A4", "player_text": "Good evening."}, "clock": 1.0,
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Welcome to the Bannered Mare."}},
    {"events": [{"text": "A bard starts to sing."}]},
    {"turn": {"speaker": "0001B07C", "player_text": "How is business?"}, "clock": 2.0,
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Business is slow this week."}},
    {"leave": ["0001B07C"], "clock": 3.0},
    {"events": [{"text": "Saadia climbs the stairs."}]},
    {"turn": {"speaker": "0001A6A4", "player_text": "Where is Saadia?"}, "clock": 4.0,
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Saadia has gone upstairs to rest."}},
    {"end": true},
    {"maintain": true},
    {"turns": {"count": 3}},
    {"conversation": {"ended": true, "participants": ["0001A6A4", "00000007"], "events": 2}},
    {"memories": {"actor": "0001A6A4", "contains": ["gone upstairs"], "count": 1}},
    {"share": {"from": "0001A6A4", "to": "0001B07C", "shared": 1, "pending": false}},
    {"memories": {"actor": "0001B07C", "contains": ["gone upstairs"], "told_by": {"gone upstairs": "0001A6A4"}}},

    {"rewind": {"game_time": 2.5, "rewound": true, "hidden_turns": 1}},
    {"turns": {"count": 2}},
    {"conversation": {"ended": false, "participants": ["0001A6A4", "0001B07C", "00000007"], "events": 1}},
    {"memories": {"actor": "0001A6A4", "absent": ["gone upstairs"]}},
    {"memories": {"actor": "0001B07C", "absent": ["gone upstairs"]}},
    {"maintain": true},
    {"memories": {"actor": "0001A6A4", "contains": ["Business is slow"], "absent": ["gone upstairs"]}},

    {"rewind": {"game_time": 10.0, "rewound": true, "restored_turns": 1}},
    {"turns": {"count": 3}},
    {"conversation": {"ended": true, "participants": ["0001A6A4", "00000007"], "events": 2}},
    {"memories": {"actor": "0001A6A4", "contains": ["gone upstairs"], "count": 1}},
    {"memories": {"actor": "0001B07C", "contains": ["gone upstairs"], "told_by": {"gone upstairs": "0001A6A4"}}},
    {"rewind": {"game_time": 10.0, "rewound": false}},

    {"rewind": {"game_time": 2.5, "rewound": true, "hidden_turns": 1}},
    {"turn": {"speaker": "0001B07C", "player_text": "Still here, Saadia?"}, "clock": 2.6,
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "I am still here."},
     "expect": {"prompt_absent": ["gone upstairs", "climbs the stairs"]}},
    {"rewind": {"game_time": 10.0, "rewound": false, "restored_turns": 0}},
    {"turns": {"count": 3}},
    {"conversation": {"ended": false, "participants": ["0001A6A4", "0001B07C", "00000007"], "events": 1}},
    {"end": true},
    {"maintain": true},
    {"memories": {"actor": "0001A6A4", "contains": ["still here"], "absent": ["gone upstairs"]}},
    {"memories": {"actor": "0001B07C", "absent": ["gone upstairs"]}}
  ]
}
```

How the fixture plays:
- The conversation starts at 1.0 (the fixture's observation). Steps without an observation take the latest seen clock: the first events 1.0, the second 3.0 (after the leave), the end 4.0, the share 4.0.
- The first `maintain` folds the ended conversation: Hulda's one summary covers turns 1-3. Hulda then tells Saadia (her memory is complete, so the share is not pending): Saadia gets a copy at 4.0.
- The rewind to 2.5 hides turn 3 (clock 4.0), the leave (3.0), the second events (3.0), the end (4.0), Saadia's copy (4.0) and Hulda's summary, since it covers turn 3. The rollback sets the conversation's `folded_through` to 2 (its last visible turn) and Hulda's mark to 0 (her hidden summary starts at turn 1), so the next `maintain` refolds turns 1-2 for her.
- The memories checks right after the rewind name no count: over HTTP the background catch-up may already have refolded.
- The rewind to 10.0 undoes the rollback: the refold is deleted; the summary, the copy, the leave, the events and the end come back. A second rewind to 10.0 changes nothing.
- The last part rewinds to 2.5 again, then plays a turn at 2.6: its prompt holds nothing from after 2.5, and it abandons the rollback. A rewind to 10.0 then restores nothing; the new turn is the conversation's third; neither NPC remembers "gone upstairs" again.

Document the new steps and the `clock` fields in `tests/hosted/fixtures/README.md`.

- [ ] **Step 2: The runner's new steps**

In `tests/hosted/HostedTestSupport.h`, `PlayMemoryFixture` (four-argument form): when the fixture has `"clock"`, the host data is `FixtureHostData()` with `clock` set to it. Each turn, leave, join and start step with a `"clock"` uses a copy of the fixture's observation with that number written at the pointer (`observation[nlohmann::json::json_pointer(pointer)] = clock`). New steps:

```cpp
        } else if (step.contains("rewind")) {
            const nlohmann::json rewind = step.at("rewind");
            const turn::RewindResult result = session.Rewind(rewind.at("game_time").get<double>());
            if (rewind.contains("rewound")) {
                CHECK(result.rewound == rewind.at("rewound").get<bool>());
            }
            if (rewind.contains("hidden_turns")) {
                CHECK(result.hiddenTurns == rewind.at("hidden_turns").get<int>());
            }
            if (rewind.contains("restored_turns")) {
                CHECK(result.restoredTurns == rewind.at("restored_turns").get<int>());
            }
        } else if (step.contains("turns")) {
            CHECK(session.Turns(id).size() == step.at("turns").at("count").get<std::size_t>());
        } else if (step.contains("conversation")) {
            const nlohmann::json expected = step.at("conversation");
            const std::optional<core::Conversation> conversation = session.Conversation(id);
            REQUIRE(conversation);
            if (expected.contains("ended")) {
                CHECK(conversation->ended == expected.at("ended").get<bool>());
            }
            if (expected.contains("participants")) {
                std::vector<std::string> ids;
                for (const core::Participant& participant : conversation->participants) {
                    ids.push_back(participant.id);
                }
                CHECK(ids == expected.at("participants").get<std::vector<std::string>>());
            }
            if (expected.contains("events")) {
                CHECK(session.Events(id).size() == expected.at("events").get<std::size_t>());
            }
        } else if (step.contains("events")) {
            session.RecordEvents(id, step.at("events").get<std::vector<nlohmann::json>>());
```

(`maintain` stays `REQUIRE(session.Maintain({}))`; with `echoedMemories` false these steps check only what does not depend on the model's words: `rewound`, the counts, `turns`, `conversation`.)

- [ ] **Step 3: Write the failing tests**

Add to `tests/hosted/HostedFixtureTests.cpp`:

```cpp
TEST_CASE("The reload fixture: a rewind hides what came after, a roll-forward restores it, playing on abandons it",
          "[hosted][fixtures][rewind]")
{
    test::PlayMemoryFixture("reload.json");
}
```

Create `tests/hosted/HostedRewindTests.cpp` with the cases the fixture does not cover:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("A rewind needs a clock and a finite game time", "[hosted][rewind]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    {
        turn::HostedSession session = turn::HostedSession::Open(
            test::HostedRunFile("hosted_rewind_noclock"), test::FixtureHostData(), test::HostedSettings(), 42, backend);
        CHECK_THROWS_WITH(session.Rewind(1.0), ContainsSubstring("game_time") && ContainsSubstring("no clock"));
    }
    turn::HostData data = test::FixtureHostData();
    data.clock = "/game_time/days_passed";
    turn::HostedSession session =
        turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_nan"), data, test::HostedSettings(), 42, backend);
    CHECK_THROWS_WITH(session.Rewind(std::numeric_limits<double>::quiet_NaN()), ContainsSubstring("game_time"));
    CHECK_THROWS_WITH(session.Rewind(std::numeric_limits<double>::infinity()), ContainsSubstring("game_time"));
    // A run with a clock but no records yet: nothing to hide.
    const turn::RewindResult nothing = session.Rewind(1.0);
    CHECK_FALSE(nothing.rewound);
    CHECK_FALSE(nothing.rollbackTime.has_value());
}

TEST_CASE("Every new record while a rollback is open abandons it", "[hosted][rewind]")
{
    // A run with a clock: one turn at 1.0, one at 2.0; rewind to 1.5; then recording events abandons the rollback,
    // and a rewind to 3.0 restores nothing.
    test::HostedScript script;
    script.actions = {R"({"action":"none","params":{}})", R"({"action":"none","params":{}})"};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostData data = test::FixtureHostData();
    data.clock = "/game_time/days_passed";
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_rewind_abandon"), data,
                                                            test::HostedSettings(), 42, backend);
    nlohmann::json at1 = test::FixtureObservation();
    at1["game_time"]["days_passed"] = 1.0;
    nlohmann::json at2 = at1;
    at2["game_time"]["days_passed"] = 2.0;
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, at1);
    turn::HostedTurnRequest first = test::Request(id, test::HULDA, "Hi.");
    first.observation = at1;
    REQUIRE(session.RunTurn(first, {}, {}));
    turn::HostedTurnRequest second = test::Request(id, test::HULDA, "Again.");
    second.observation = at2;
    REQUIRE(session.RunTurn(second, {}, {}));

    CHECK(session.Rewind(1.5).hiddenTurns == 1);
    session.RecordEvents(id, {nlohmann::json{{"text", "A door opens."}}});
    const turn::RewindResult forward = session.Rewind(3.0);
    CHECK_FALSE(forward.rewound);
    CHECK(forward.restoredTurns == 0);
    CHECK(session.Turns(id).size() == 1);
}
```

- [ ] **Step 4: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`Rewind` is not a member of `HostedSession`).

- [ ] **Step 5: Rewind and abandoning**

In `src/turn/HostedSession.h`, before the class, `RewindResult` as in **Produces** with a comment; the public method:

```cpp
    // The run as it was at game time `gameTime` (rollback spec, section 3): the host calls it on every save load. It
    // undoes the open rollback, if any, then applies one to `gameTime` when that is before the latest live clock; a
    // rewind to the open rollback's own time changes nothing. Hidden records are kept; a later rewind restores them
    // until a new record abandons them. The background upkeep then refolds what NPCs are missing. Throws DataError
    // "game_time: ..." when the host data names no clock or `gameTime` is not finite.
    RewindResult Rewind(double gameTime);
```

In `src/turn/HostedSession.cpp`:

```cpp
RewindResult HostedSession::Rewind(double gameTime)
{
    if (!_world.HasClock()) {
        throw core::DataError("game_time: the host data names no clock, so the run cannot be rewound");
    }
    if (!std::isfinite(gameTime)) {
        throw core::DataError("game_time: expected a finite number");
    }
    // The same time as the open rollback: nothing to do.
    const std::optional<store::Rollback> open = _store.OpenRollback();
    if (open && open->gameTime == gameTime) {
        return {false, 0, 0, gameTime};
    }
    // Undo the open rollback, then apply one when the time is before the latest live record.
    RewindResult result;
    if (open) {
        result.restoredTurns = _store.UndoRollback().restoredTurns;
    }
    const std::optional<double> latest = _store.LatestClock();
    if (latest && gameTime < *latest) {
        result.hiddenTurns = _store.ApplyRollback(gameTime).hiddenTurns;
        result.rollbackTime = gameTime;
    }
    // The host sees the net change. Both rollbacks hide every turn clocked after their time, so one of the two sets
    // holds the other: a turn restored by the undo and hidden again by the new rollback counts as neither.
    const int both = std::min(result.restoredTurns, result.hiddenTurns);
    result.restoredTurns -= both;
    result.hiddenTurns -= both;
    // Rewound: the open rollback changed (one was undone, applied, or both; the same time returned early above).
    result.rewound = open.has_value() || result.rollbackTime.has_value();
    SyncHistory();
    return result;
}
```

(`SyncHistory` is E1's full index sync, the private method `Open` calls; a rewind changes turns and summaries both.)

Abandoning: add a private helper with several callers:

```cpp
// A new record while a rollback is open: the hidden future is abandoned first (rollback spec, "Playing on").
void HostedSession::AbandonRollbackForNewRecord()
{
    if (_store.OpenRollback()) {
        _store.AbandonRollback();
    }
}
```

and call it in `StartConversation`, `RecordEvents`, `SetParticipants`, `EndConversation` after their checks and before their first write; in `RunTurn` after the request's checks and before the catch-up; in `ShareMemories` after its checks. `Maintain` never calls it.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[rewind]"`, `"[memory]"`, `"[hosted]"`, `"[hosted-server]"`, `"[turn]"`, `"[replay]"`, and `ctest --preset tests`.
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/
git commit -m "Rewind: a hosted run as it was at a game time; a new record abandons the hidden future; R6's reload fixture"
```

---

