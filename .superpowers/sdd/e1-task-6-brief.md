### Task 6: Sharing an NPC's memories of a conversation

**Files:**
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Modify: `tests/hosted/fixtures/three_actors.json`, `tests/hosted/fixtures/README.md`, `tests/hosted/HostedTestSupport.h`
- Test: `tests/hosted/HostedMemoryTests.cpp`

**Interfaces:**
- Consumes: Tasks 1-5 (`NpcWitnesses`, `RememberedThrough`, `ActorSummaries`, `AddActorSummary`, pending shares).
- Produces:
  - `struct turn::ShareResult { int shared = 0; bool pending = false; };` (in `turn/HostedSession.h`)
  - `ShareResult HostedSession::ShareMemories(const std::string& fromActorId, const std::string& toActorId, const std::string& conversationId)`
  - `HostedSession` private: `int ApplyShare(const std::string& fromActorId, const std::string& toActorId, const std::string& conversationId)` (copies what is not copied yet; returns how many; callers: `ShareMemories`, `Maintain`).
  - `Maintain` applies pending shares whose teller's memory is complete.
  - The fixture step `{"share": {"from", "to", "shared"?, "pending"?}}` and memories checks `"told_by": {"<substring>": "<teller id>"}`.

- [ ] **Step 1: The fixture's share steps**

Append to `three_actors.json`'s steps:

```json
    {"share": {"from": "0001A6A4", "to": "0001B07C", "pending": false}},
    {"memories": {"actor": "0001B07C", "contains": ["takes bribes"], "told_by": {"takes bribes": "0001A6A4"}}},
    {"turn": {"speaker": "0001B07C", "player_text": "Anything new about the guard captain?"},
     "model": {"resolve": "{\"action\":\"none\",\"params\":{}}", "line": "Hulda says he takes bribes."},
     "expect": {"prompt_contains": ["(told by Hulda)", "takes bribes"]}},
    {"share": {"from": "0001A6A4", "to": "0001B07C", "shared": 0, "pending": false}},
    {"memories": {"actor": "0001B07D", "absent": ["takes bribes"]}}
```

Document in the fixtures README: `{"share": {"from", "to", "conversation"?, "shared"?, "pending"?}}` (`conversation` defaults to the fixture's first conversation; `shared` and `pending` are checked when given) and the memories check `"told_by"`: for each substring, the memory containing it must be told by that teller.

- [ ] **Step 2: Write the failing tests**

In `PlayMemoryFixture` (`HostedTestSupport.h`), keep the first conversation's id (`const std::string firstId = id;` after starting it) and add the share step and the `told_by` check:

```cpp
        } else if (step.contains("share")) {
            const nlohmann::json& share = step.at("share");
            const turn::ShareResult result =
                session.ShareMemories(share.at("from").get<std::string>(), share.at("to").get<std::string>(),
                                      share.value("conversation", firstId));
            if (share.contains("shared")) {
                CHECK(result.shared == share.at("shared").get<int>());
            }
            if (share.contains("pending")) {
                CHECK(result.pending == share.at("pending").get<bool>());
            }
```

and, in the memories branch:

```cpp
            for (const auto& [substring, teller] : check.value("told_by", nlohmann::json::object()).items()) {
                const auto it = std::find_if(memories.begin(), memories.end(), [&substring](const core::Summary& memory) {
                    return memory.text.find(substring) != std::string::npos;
                });
                REQUIRE(it != memories.end());
                CHECK(it->toldBy == teller.get<std::string>());
            }
```

(`items()` on a const json: iterate `for (const auto& item : obj.items())` with `item.key()` and `item.value()` if structured bindings do not compile.)

Add to `tests/hosted/HostedMemoryTests.cpp`:

```cpp
TEST_CASE("A share made while the conversation is open waits, then applies once it ended and the teller caught up",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_pending"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Any news?"), {}, {}));

    const turn::ShareResult early = session.ShareMemories(test::HULDA, test::SAADIA, id);
    CHECK(early.pending);
    CHECK(early.shared == 0);
    CHECK(session.Memories(test::SAADIA).empty());

    session.EndConversation(id);
    REQUIRE(session.Maintain({}));
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].toldBy == test::HULDA);
    CHECK(saadia[0].text.find("poisoned") != std::string::npos);
    // Applied once: another pass copies nothing more.
    REQUIRE(session.Maintain({}));
    CHECK(session.Memories(test::SAADIA).size() == 1);
}

TEST_CASE("A share refuses a teller who witnessed nothing, the same actor, and an unknown conversation",
          "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello."};
    test::ScriptedBackend backend(16384, test::MemoryEchoResponder(script));
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_refused"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    CHECK_THROWS_WITH(session.ShareMemories(test::SAADIA, test::HULDA, id), ContainsSubstring("from"));
    CHECK_THROWS_WITH(session.ShareMemories(test::AELA, test::HULDA, id), ContainsSubstring("from")); // The player.
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, test::HULDA, id), ContainsSubstring("to"));
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, "", id), ContainsSubstring("to"));
    CHECK_THROWS_WITH(session.ShareMemories(test::HULDA, test::SAADIA, "c9"), ContainsSubstring("conversation_id"));
}

TEST_CASE("A shared memory is a snapshot: the teller's later condense leaves it as it was", "[hosted][memory]")
{
    // Folds echo what they read; a condense writes "Condensed." (an echo would stay over the small cap).
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"The well is poisoned.", std::string(200, 'x'), std::string(200, 'y')};
    const test::ScriptedBackend::Responder echo = test::MemoryEchoResponder(script);
    test::ScriptedBackend backend(16384, [&echo](const inference::Prompt& prompt, const inference::GenerationSpec& spec) {
        return prompt.messages.back().content.find("Condense what") != std::string::npos ? std::string("Condensed.")
                                                                                         : echo(prompt, spec);
    });
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = 64;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_share_snapshot"),
                                                            test::FixtureHostData(), settings, 42, backend);

    // The first conversation, remembered by Hulda and shared to Saadia.
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "News?"), {}, {}));
    session.EndConversation(first);
    REQUIRE(session.Maintain({}));
    REQUIRE(session.ShareMemories(test::HULDA, test::SAADIA, first).shared == 1);
    const core::Summary copy = session.Memories(test::SAADIA).at(0);

    // Two more conversations put Hulda's memories over summary_cap 64: the upkeep condenses them.
    for (int i = 0; i < 2; ++i) {
        const std::string next = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        REQUIRE(session.RunTurn(test::Request(next, test::HULDA, "More?"), {}, {}));
        session.EndConversation(next);
        REQUIRE(session.Maintain({}));
    }
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    const auto shown = std::count_if(hulda.begin(), hulda.end(), [](const core::Summary& s) { return !s.superseded; });
    CHECK(shown == 1);
    REQUIRE_FALSE(hulda.empty());
    CHECK(hulda.front().superseded); // Her first summary is kept, superseded.

    // Saadia's copy is as it was.
    const std::vector<core::Summary> saadia = session.Memories(test::SAADIA);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].text == copy.text);
    CHECK(saadia[0].toldBy == test::HULDA);
    CHECK(saadia[0].sourceId == copy.sourceId);
}
```

Add `#include <algorithm>` for `std::count_if`.

- [ ] **Step 3: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`ShareMemories` is not a member of `HostedSession`).

- [ ] **Step 4: Sharing**

In `src/turn/HostedSession.h`, before `class HostedSession`:

```cpp
// What a share did: how many summaries it copied now, and whether it waits for the teller's memory (per-speaker
// memory spec, section 2).
struct ShareResult {
    int shared = 0;
    bool pending = false;
};
```

and the public method:

```cpp
    // `toActorId` learns what `fromActorId` remembers of conversation `conversationId`: copies of `fromActorId`'s own
    // summaries of it (superseded or not, shared copies excluded), each told by `fromActorId`, oldest first. A copy is
    // a snapshot, and a summary already copied to `toActorId` is not copied again. While the conversation is open or
    // `fromActorId` is behind in it, the share waits and Maintain applies it once the conversation ended and
    // `fromActorId` caught up. Throws DataError "conversation_id: ..." for an unknown conversation, "from: ..." when
    // `fromActorId` witnessed none of its turns as an NPC, and "to: ..." for an empty `toActorId` or the teller itself.
    ShareResult ShareMemories(const std::string& fromActorId, const std::string& toActorId,
                              const std::string& conversationId);
```

In `src/turn/HostedSession.cpp`:

```cpp
ShareResult HostedSession::ShareMemories(const std::string& fromActorId, const std::string& toActorId,
                                         const std::string& conversationId)
{
    // The conversation, the teller (an NPC witness of it) and the listener (another actor).
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    if (!conversation) {
        throw core::DataError(std::format("conversation_id: there is no conversation '{}'", conversationId));
    }
    const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversationId);
    const std::vector<std::string> npcs = NpcWitnesses(turns);
    if (std::find(npcs.begin(), npcs.end(), fromActorId) == npcs.end()) {
        throw core::DataError(std::format("from: '{}' witnessed no turn of conversation '{}' as an NPC", fromActorId,
                                          conversationId));
    }
    if (toActorId.empty() || toActorId == fromActorId) {
        throw core::DataError(std::format("to: '{}' must be another actor than the teller '{}'", toActorId, fromActorId));
    }

    // The teller's memory of it is complete once the conversation ended and the teller caught up to its last turn;
    // until then the share waits.
    const bool complete =
        conversation->ended && _store.RememberedThrough(conversationId, fromActorId) >= turns.back().number;
    if (!complete) {
        _store.AddPendingShare({0, fromActorId, toActorId, conversationId});
        return {0, true};
    }
    return {ApplyShare(fromActorId, toActorId, conversationId), false};
}

int HostedSession::ApplyShare(const std::string& fromActorId, const std::string& toActorId,
                              const std::string& conversationId)
{
    // What the listener already has from these summaries: copies are keyed by their source.
    std::set<std::int64_t> copied;
    for (const core::Summary& summary : _store.ActorSummaries(toActorId, true)) {
        if (summary.sourceId != 0) {
            copied.insert(summary.sourceId);
        }
    }
    // The teller's own summaries of the conversation (what it saw, not what it was told), as snapshots.
    int shared = 0;
    for (const core::Summary& summary : _store.ActorSummaries(fromActorId, true)) {
        if (summary.conversationId != conversationId || !summary.toldBy.empty() || copied.contains(summary.id)) {
            continue;
        }
        core::Summary copy;
        copy.fromTurn = summary.fromTurn;
        copy.toTurn = summary.toTurn;
        copy.text = summary.text;
        copy.actorId = toActorId;
        copy.conversationId = conversationId;
        copy.toldBy = fromActorId;
        copy.sourceId = summary.id;
        _store.AddActorSummary(copy);
        ++shared;
    }
    if (shared > 0) {
        SyncHistory(); // The copies are searchable by their new owner.
    }
    return shared;
}
```

In `Maintain`, after the conversations loop:

```cpp
    // Shares waiting for their teller: applied once the conversation ended and the teller caught up, then removed.
    for (const core::PendingShare& share : _store.PendingShares()) {
        const std::optional<core::Conversation> conversation = _store.Conversation(share.conversationId);
        const std::vector<core::HostedTurn> turns = _store.HostedTurns(share.conversationId);
        const bool complete = conversation && conversation->ended && !turns.empty() &&
                              _store.RememberedThrough(share.conversationId, share.fromActor) >= turns.back().number;
        if (complete) {
            ApplyShare(share.fromActor, share.toActor, share.conversationId);
            _store.RemovePendingShare(share.id);
        }
    }
```

Add `#include <set>`.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[memory]"`, `"[hosted]"`, `"[hosted-server]"`.
Expected: `All tests passed` for each.

- [ ] **Step 6: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/
git commit -m "Sharing: an NPC tells another what it remembers of a conversation, as snapshots, waiting until its memory is complete"
```

---

