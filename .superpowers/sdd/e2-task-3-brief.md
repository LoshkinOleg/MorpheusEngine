### Task 3: The rollback transactions in the store

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Create: `tests/store/RollbackStoreTests.cpp`

**Interfaces:**
- Consumes: Task 2's schema and readers.
- Produces:
  - `struct store::Rollback { std::int64_t id = 0; double gameTime = 0.0; };` and `struct store::RollbackCounts { int hiddenTurns = 0; int restoredTurns = 0; };`
  - `std::optional<Rollback> RunStore::OpenRollback()` (the one open rollback; StoreError naming the file and `rollbacks` when there are several)
  - `RollbackCounts RunStore::ApplyRollback(double gameTime)` (ContractError when one is open)
  - `RollbackCounts RunStore::UndoRollback()` (ContractError when none is open)
  - `void RunStore::AbandonRollback()` (ContractError when none is open)
  - Every summary written while a rollback is open stores its id in `made_in` (Task 2's stamp, first exercised here).

- [ ] **Step 1: Write the failing tests**

Create `tests/store/RollbackStoreTests.cpp`. Its helper builds a run through the store with clocks, so each test reads what a rollback hid:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "core/TurnRecord.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A hosted run's meta and host data, as the hosted store tests make them.
store::RunMeta Meta()
{
    store::RunMeta meta;
    meta.kind = store::RunKind::Hosted;
    meta.worldId = "hosted";
    meta.worldHash = "0123456789abcdef";
    meta.modelId = "model";
    meta.rngSeed = 42;
    meta.settings = nlohmann::json::object();
    return meta;
}

// Turn `number` of conversation `id` at `clock`: npc-1 speaks to npc-2 and the player.
core::HostedTurn Turn(int number, const std::string& id, double clock)
{
    core::HostedTurn turn;
    turn.number = number;
    turn.conversationId = id;
    turn.speakerId = "npc-1";
    turn.listenerIds = {"npc-2", "player"};
    turn.observation = {{"actors",
                         {{{"id", "npc-1"}, {"name", "Hulda"}, {"is_player", false}},
                          {{"id", "npc-2"}, {"name", "Saadia"}, {"is_player", false}},
                          {{"id", "player"}, {"name", "Aela"}, {"is_player", true}}}}};
    turn.line = "Line " + std::to_string(number) + ".";
    turn.clock = clock;
    return turn;
}

// npc-`actor`'s fold of conversation `id` over `from`..`to`.
core::Summary Fold(const std::string& actor, const std::string& id, int from, int to)
{
    core::Summary summary;
    summary.fromTurn = from;
    summary.toTurn = to;
    summary.text = actor + " remembers " + std::to_string(from) + "-" + std::to_string(to) + ".";
    summary.actorId = actor;
    summary.conversationId = id;
    return summary;
}

// A run with conversation c1: turns 1-4 at clocks 1-4, an event at 2.5, a leave at 3.5, ended at 4.5; npc-1 folded
// turns 1-2 and 3-4 (marks at 4), npc-2 folded 1-4 in one summary.
std::filesystem::path Run(const char* name, std::string& id)
{
    const std::filesystem::path file = test::FreshTempDir(name) / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, Meta(), nlohmann::json{{"catalog", nlohmann::json::object()}});
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"npc-2", "Saadia", ""}, {"player", "Aela", ""}};
    id = store.CreateConversation(participants, 0.5);
    store.CommitHostedTurn(Turn(1, id, 1.0));
    store.CommitHostedTurn(Turn(2, id, 2.0));
    store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "A dragon roars."}}}, 2.5);
    store.CommitHostedTurn(Turn(3, id, 3.0));
    store.SetParticipants(id, {participants[0], participants[2]}, 3.5);
    store.CommitHostedTurn(Turn(4, id, 4.0));
    store.EndConversation(id, 4.5);
    store.FoldConversation(id, Fold("npc-1", id, 1, 2));
    store.FoldConversation(id, Fold("npc-1", id, 3, 4));
    store.RememberFold(Fold("npc-2", id, 1, 4));
    return file;
}

} // namespace

TEST_CASE("A rollback hides what came after its time, moves the marks back, and an undo restores it exactly",
          "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_apply_undo", id));
    const store::RollbackCounts applied = store.ApplyRollback(2.7);
    CHECK(applied.hiddenTurns == 2);
    REQUIRE(store.OpenRollback().has_value());
    CHECK(store.OpenRollback()->gameTime == 2.7);

    // Turns 3-4, the leave and the end are hidden; the event at 2.5 stays.
    CHECK(store.HostedTurns(id).size() == 2);
    CHECK(store.LatestHostedTurn() == 2);
    CHECK(store.ConversationEvents(id).size() == 1);
    CHECK(store.Conversation(id)->participants.size() == 3);
    CHECK_FALSE(store.Conversation(id)->ended);
    CHECK(store.LatestClock() == 2.5);

    // npc-1's 3-4 is hidden, its 1-2 stays; npc-2's 1-4 straddles: hidden, and its mark moves back before it.
    const std::vector<core::Summary> npc1 = store.ActorSummaries("npc-1", false);
    REQUIRE(npc1.size() == 1);
    CHECK(npc1[0].toTurn == 2);
    CHECK(store.ActorSummaries("npc-2", true).empty());
    CHECK(store.Conversation(id)->foldedThrough == 2);
    CHECK(store.RememberedThrough(id, "npc-1") == 2);
    CHECK(store.RememberedThrough(id, "npc-2") == 0);

    // A summary written while the rollback is open belongs to it.
    store.RememberFold(Fold("npc-2", id, 1, 2));
    CHECK(store.ActorSummaries("npc-2", false).at(0).madeIn == store.OpenRollback()->id);

    // Undo: everything back as it was; the refold is gone.
    const store::RollbackCounts undone = store.UndoRollback();
    CHECK(undone.restoredTurns == 2);
    CHECK_FALSE(store.OpenRollback().has_value());
    CHECK(store.HostedTurns(id).size() == 4);
    CHECK(store.Conversation(id)->ended);
    CHECK(store.Conversation(id)->participants.size() == 2);
    CHECK(store.Conversation(id)->foldedThrough == 4);
    CHECK(store.RememberedThrough(id, "npc-2") == 4);
    const std::vector<core::Summary> npc2 = store.ActorSummaries("npc-2", true);
    REQUIRE(npc2.size() == 1);
    CHECK(npc2[0].toTurn == 4);
    CHECK(store.LatestClock() == 4.5);
}

TEST_CASE("A rollback shows again what a hidden condense superseded", "[store][rollback]")
{
    std::string id;
    const std::filesystem::path file = Run("rollback_condense", id);
    {
        store::RunStore store = store::RunStore::Open(file);
        core::Summary condensed;
        condensed.fromTurn = 1;
        condensed.toTurn = 4;
        condensed.text = "npc-1 condensed.";
        condensed.actorId = "npc-1";
        store.CondenseActorSummaries("npc-1", condensed);
        REQUIRE(store.ActorSummaries("npc-1", false).size() == 1);
    }
    store::RunStore store = store::RunStore::Open(file);
    store.ApplyRollback(2.7);
    // The condense covers hidden turns: hidden; the 1-2 it superseded is shown again, the 3-4 stays hidden.
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].text == "npc-1 remembers 1-2.");
    CHECK(store.RememberedThrough(id, "npc-1") == 2);
}

TEST_CASE("Abandoning keeps the hidden rows hidden for good and the refolds as ordinary memories", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_abandon", id));
    store.ApplyRollback(2.7);
    store.RememberFold(Fold("npc-2", id, 1, 2));
    store.AbandonRollback();
    CHECK_FALSE(store.OpenRollback().has_value());
    CHECK(store.HostedTurns(id).size() == 2);
    CHECK(store.ActorSummaries("npc-2", false).at(0).madeIn == 0);
    CHECK(store.NextHostedTurn() == 5);
    CHECK_THROWS_AS(store.UndoRollback(), core::ContractError);
}

TEST_CASE("A share applied after the rollback's time is pending again; one made after it is hidden", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_shares", id));
    core::PendingShare early;
    early.fromActor = "npc-1";
    early.toActor = "npc-3";
    early.conversationId = id;
    early.clock = 2.0;
    const std::int64_t earlyId = store.AddPendingShare(early);
    store.MarkShareApplied(earlyId, 4.6);
    core::Summary copy = Fold("npc-3", id, 1, 2);
    copy.toldBy = "npc-1";
    copy.clock = 4.6;
    store.AddActorSummary(copy);
    core::PendingShare late = early;
    late.clock = 4.7;
    store.AddPendingShare(late);
    CHECK(store.PendingShares().size() == 1); // The late one.

    store.ApplyRollback(2.7);
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == earlyId);
    CHECK(store.ActorSummaries("npc-3", true).empty());

    store.UndoRollback();
    CHECK(store.PendingShares().size() == 1);
    CHECK(store.ActorSummaries("npc-3", true).size() == 1);
}

TEST_CASE("Rollback transactions refuse a second open rollback and a missing one", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_contracts", id));
    CHECK_THROWS_AS(store.UndoRollback(), core::ContractError);
    CHECK_THROWS_AS(store.AbandonRollback(), core::ContractError);
    store.ApplyRollback(2.7);
    CHECK_THROWS_AS(store.ApplyRollback(1.5), core::ContractError);
}

TEST_CASE("Damaged rollback rows are StoreErrors naming the file", "[store][rollback]")
{
    std::string id;
    const std::filesystem::path file = Run("rollback_damaged", id);
    {
        store::RunStore store = store::RunStore::Open(file);
        store.ApplyRollback(2.7);
    }
    {
        store::Database db = store::Database::Open(file, false);
        db.Exec("UPDATE rollbacks SET saved_marks = 'nope'");
    }
    store::RunStore store = store::RunStore::Open(file);
    CHECK_THROWS_WITH(store.UndoRollback(), ContainsSubstring("run.db") && ContainsSubstring("saved_marks"));
}

TEST_CASE("A hidden row naming no rollback, or two open rollbacks, is a StoreError on open", "[store][rollback]")
{
    std::string id;
    const std::filesystem::path dangling = Run("rollback_dangling", id);
    {
        store::Database db = store::Database::Open(dangling, false);
        db.Exec("UPDATE hosted_turns SET hidden_by = 7 WHERE number = 4");
    }
    CHECK_THROWS_WITH(store::RunStore::Open(dangling),
                      ContainsSubstring("run.db") && ContainsSubstring("hosted_turns") && ContainsSubstring("hidden_by"));

    const std::filesystem::path twoOpen = Run("rollback_two_open", id);
    {
        store::Database db = store::Database::Open(twoOpen, false);
        db.Exec("INSERT INTO rollbacks (game_time, status, saved_marks) VALUES (1.0, 'open', '{}'), "
                "(2.0, 'open', '{}')");
    }
    CHECK_THROWS_WITH(store::RunStore::Open(twoOpen), ContainsSubstring("run.db") && ContainsSubstring("rollbacks"));
}
```

(The helpers mirror `tests/store/HostedStoreTests.cpp`'s `HostedMeta()`, `TestHostData()` and `TestHostedTurn(number, conversationId)`, which live in that file's anonymous namespace: if `CreateHosted` or `CommitHostedTurn` checks fields this `Turn` leaves empty, copy the values `TestHostedTurn` sets, keeping the clock.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`ApplyRollback` is not a member of `RunStore`).

- [ ] **Step 3: The transactions**

In `src/store/RunStore.h`, before `class RunStore`: `Rollback` and `RollbackCounts` as in **Produces**, with comments. Declare the four methods with comments taken from the spec's section 3.

In `src/store/RunStore.cpp`:

```cpp
std::optional<Rollback> RunStore::OpenRollback()
{
    RequireHosted("OpenRollback");
    Statement query = _db.Prepare("SELECT id, game_time FROM rollbacks WHERE status = 'open' ORDER BY id");
    std::optional<Rollback> open;
    while (query.Step()) {
        if (open) {
            throw StoreError(std::format("'{}': rollbacks: more than one rollback is open", core::PathText(_file)));
        }
        open = Rollback{query.ColumnInt(0), query.ColumnDouble(1)};
    }
    return open;
}

RollbackCounts RunStore::ApplyRollback(double gameTime)
{
    RequireHosted("ApplyRollback");
    MORPHEUS_REQUIRE(!OpenRollback(), "RunStore::ApplyRollback: a rollback is already open");
    RollbackCounts counts;
    _db.Transaction([&] {
        // The marks it moves, saved first: every conversation's folded mark and every NPC's.
        nlohmann::json saved = {{"folded_through", nlohmann::json::object()}, {"remembered", nlohmann::json::array()}};
        Statement folded = _db.Prepare("SELECT id, folded_through FROM conversations");
        while (folded.Step()) {
            saved["folded_through"][folded.ColumnText(0)] = folded.ColumnInt(1);
        }
        Statement remembered = _db.Prepare("SELECT conversation_id, actor_id, remembered_through FROM actor_memory");
        while (remembered.Step()) {
            saved["remembered"].push_back(
                {remembered.ColumnText(0), remembered.ColumnText(1), remembered.ColumnInt(2)});
        }
        _db.Prepare("INSERT INTO rollbacks (game_time, status, saved_marks) VALUES (?1, 'open', ?2)")
            .Bind(1, gameTime)
            .Bind(2, saved.dump())
            .Run();
        Statement inserted = _db.Prepare("SELECT last_insert_rowid()");
        const bool hasRow = inserted.Step();
        MORPHEUS_REQUIRE(hasRow, "ApplyRollback: last_insert_rowid returned no row");
        const std::int64_t id = inserted.ColumnInt(0);

        // Records clocked after the time, and conversations started after it with everything in them.
        Statement hiddenTurns = _db.Prepare("SELECT COUNT(*) FROM hosted_turns WHERE hidden_by = 0 AND clock > ?1");
        hiddenTurns.Bind(1, gameTime);
        hiddenTurns.Step();
        counts.hiddenTurns = static_cast<int>(hiddenTurns.ColumnInt(0));
        for (const char* table : {"hosted_turns", "conversation_events", "conversation_changes", "conversations"}) {
            _db.Prepare(std::format("UPDATE {} SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2", table))
                .Bind(1, id)
                .Bind(2, gameTime)
                .Run();
        }
        // Shared copies and pending shares made after it; shares applied after it are pending again.
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2").Bind(1, id).Bind(2, gameTime).Run();
        _db.Prepare("UPDATE pending_shares SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2")
            .Bind(1, id)
            .Bind(2, gameTime)
            .Run();
        _db.Prepare("UPDATE pending_shares SET unapplied_by = ?1 WHERE hidden_by = 0 AND applied = 1 AND unapplied_by = 0 "
                    "AND applied_clock > ?2")
            .Bind(1, id)
            .Bind(2, gameTime)
            .Run();
        // Summaries covering a hidden turn: past their conversation's last visible turn, or, with no conversation,
        // past the run's.
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND conversation_id <> '' AND to_turn > "
                    "COALESCE((SELECT MAX(number) FROM hosted_turns t WHERE t.conversation_id = summaries.conversation_id "
                    "AND t.hidden_by = 0), 0)")
            .Bind(1, id)
            .Run();
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND conversation_id = '' AND to_turn > "
                    "COALESCE((SELECT MAX(number) FROM hosted_turns WHERE hidden_by = 0), 0)")
            .Bind(1, id)
            .Run();
        // Marks back to each conversation's last visible turn, and an NPC's behind its own hidden fold summaries
        // that cover a visible turn (the catch-up refolds those turns).
        _db.Prepare("UPDATE conversations SET folded_through = MIN(folded_through, COALESCE((SELECT MAX(number) FROM "
                    "hosted_turns t WHERE t.conversation_id = conversations.id AND t.hidden_by = 0), 0))")
            .Run();
        _db.Prepare("UPDATE actor_memory SET remembered_through = MIN(remembered_through, COALESCE((SELECT MAX(number) "
                    "FROM hosted_turns t WHERE t.conversation_id = actor_memory.conversation_id AND t.hidden_by = 0), 0))")
            .Run();
        _db.Prepare("UPDATE actor_memory SET remembered_through = MIN(remembered_through, COALESCE((SELECT "
                    "MIN(s.from_turn) - 1 FROM summaries s WHERE s.hidden_by = ?1 AND s.actor_id = actor_memory.actor_id "
                    "AND s.conversation_id = actor_memory.conversation_id AND s.told_by = '' AND s.from_turn <= "
                    "COALESCE((SELECT MAX(number) FROM hosted_turns t WHERE t.conversation_id = actor_memory.conversation_id "
                    "AND t.hidden_by = 0), 0)), remembered_through))")
            .Bind(1, id)
            .Run();
    });
    return counts;
}

RollbackCounts RunStore::UndoRollback()
{
    RequireHosted("UndoRollback");
    const std::optional<Rollback> open = OpenRollback();
    MORPHEUS_REQUIRE(open.has_value(), "RunStore::UndoRollback: no rollback is open");
    // The saved marks, read before anything changes: damaged ones stop the undo.
    Statement marks = _db.Prepare("SELECT saved_marks FROM rollbacks WHERE id = ?1");
    marks.Bind(1, open->id);
    marks.Step();
    const nlohmann::json saved = nlohmann::json::parse(marks.ColumnText(0), nullptr, false);
    if (saved.is_discarded() || !saved.contains("folded_through") || !saved.contains("remembered")) {
        throw StoreError(std::format("'{}': rollback {}: saved_marks is damaged", core::PathText(_file), open->id));
    }
    RollbackCounts counts;
    _db.Transaction([&] {
        Statement restored = _db.Prepare("SELECT COUNT(*) FROM hosted_turns WHERE hidden_by = ?1");
        restored.Bind(1, open->id);
        restored.Step();
        counts.restoredTurns = static_cast<int>(restored.ColumnInt(0));
        // Refolds made while it was open go, with their vectors; what it hid comes back.
        _db.Prepare("DELETE FROM embeddings WHERE kind = 'summary' AND CAST(ref_id AS INTEGER) IN (SELECT id FROM "
                    "summaries WHERE made_in = ?1)")
            .Bind(1, open->id)
            .Run();
        _db.Prepare("DELETE FROM summary_supersessions WHERE condensed_id IN (SELECT id FROM summaries WHERE "
                    "made_in = ?1)")
            .Bind(1, open->id)
            .Run();
        _db.Prepare("DELETE FROM summaries WHERE made_in = ?1").Bind(1, open->id).Run();
        for (const char* table : {"hosted_turns", "conversation_events", "conversation_changes", "conversations",
                                  "summaries", "pending_shares"}) {
            _db.Prepare(std::format("UPDATE {} SET hidden_by = 0 WHERE hidden_by = ?1", table)).Bind(1, open->id).Run();
        }
        _db.Prepare("UPDATE pending_shares SET unapplied_by = 0 WHERE unapplied_by = ?1").Bind(1, open->id).Run();
        // The saved marks, exactly.
        for (auto it = saved.at("folded_through").begin(); it != saved.at("folded_through").end(); ++it) {
            _db.Prepare("UPDATE conversations SET folded_through = ?1 WHERE id = ?2")
                .Bind(1, it.value().get<int>())
                .Bind(2, it.key())
                .Run();
        }
        _db.Prepare("DELETE FROM actor_memory").Run();
        for (const nlohmann::json& mark : saved.at("remembered")) {
            _db.Prepare("INSERT INTO actor_memory (conversation_id, actor_id, remembered_through) VALUES (?1, ?2, ?3)")
                .Bind(1, mark.at(0).get<std::string>())
                .Bind(2, mark.at(1).get<std::string>())
                .Bind(3, mark.at(2).get<int>())
                .Run();
        }
        _db.Prepare("UPDATE rollbacks SET status = 'undone' WHERE id = ?1").Bind(1, open->id).Run();
    });
    return counts;
}

void RunStore::AbandonRollback()
{
    RequireHosted("AbandonRollback");
    const std::optional<Rollback> open = OpenRollback();
    MORPHEUS_REQUIRE(open.has_value(), "RunStore::AbandonRollback: no rollback is open");
    // What it hid stays hidden for good; its refolds become ordinary memories; shares it unapplied are pending.
    _db.Transaction([&] {
        _db.Prepare("UPDATE summaries SET made_in = 0 WHERE made_in = ?1").Bind(1, open->id).Run();
        _db.Prepare("UPDATE pending_shares SET applied = 0, applied_clock = NULL, unapplied_by = 0 WHERE unapplied_by = ?1")
            .Bind(1, open->id)
            .Run();
        _db.Prepare("UPDATE rollbacks SET status = 'abandoned', saved_marks = '{}' WHERE id = ?1").Bind(1, open->id).Run();
    });
}
```

`RunStore::Open` of a hosted run checks the rollback rows once the schema is current (after any upgrade), so damaged rows fail at open (spec, "Errors"):

```cpp
    // Every hidden row names a rollback, and at most one rollback is open.
    if (store._meta.kind == RunKind::Hosted) {
        for (const char* table : {"hosted_turns", "conversation_events", "conversation_changes", "conversations",
                                  "summaries", "pending_shares"}) {
            Statement dangling = store._db.Prepare(std::format(
                "SELECT COUNT(*) FROM {} WHERE hidden_by <> 0 AND hidden_by NOT IN (SELECT id FROM rollbacks)", table));
            dangling.Step();
            if (dangling.ColumnInt(0) > 0) {
                throw StoreError(std::format("'{}': {}: hidden_by names no rollback", core::PathText(file), table));
            }
        }
        store.OpenRollback(); // Throws when more than one is open.
    }
```

(Place it where `Open` already validates a hosted run after upgrading; use the local names `Open` has there.)

Summaries written while a rollback is open belong to it (spec, "Refolds belong to the rollback"). Task 2's summary writers already stamp `made_in` through the private `OpenRollbackId()`; this task's first test is the first that opens a rollback, so it checks that stamp. If `OpenRollback()` and `OpenRollbackId()` now read the same rows, make `OpenRollbackId()` return `OpenRollback()`'s id (0 when none) so the "more than one open" check covers both.

Notes for the implementer:
- `MIN(a, b)` with two arguments is SQLite's scalar minimum; `MIN(col)` over rows is the aggregate. Read each statement against the spec's rule; a test per rule is above.
- The new rollback's id is read with `SELECT last_insert_rowid()`, as `AddActorSummary` reads a summary's.
- `Bind(1, gameTime)` with a `double` must pick Task 2's `Bind(int, double)` overload, not the `std::int64_t` one; Task 2 adds both `Bind(int, double)` and `Bind(int, std::optional<double>)`.
- `json::at(0)` on an array element works; the marks are saved as `[conversation, actor, through]` triples.
- A hidden conversation's turns and events have later clocks, so they are hidden by their own clocks; the conversation row is hidden too, so `Conversations()` leaves it out.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[rollback]"`, `"[store]"`, `"[hosted]"`.
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/store/RunStore.h src/store/RunStore.cpp tests/store/RollbackStoreTests.cpp
git commit -m "Rollback transactions: apply hides and moves marks back, undo restores exactly, abandon keeps the hidden rows hidden"
```

---

