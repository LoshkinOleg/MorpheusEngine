### Task 2: Store version 6: clocks, conversation history, superseded_by, visible-only readers

**Files:**
- Modify: `src/core/HostedRecord.h`, `src/core/TurnRecord.h`
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Modify: `src/turn/HostedSession.cpp` (passes clocks; uses the renamed turn-number methods)
- Test: `tests/store/HostedStoreTests.cpp`, `tests/store/RunStoreTests.cpp` (version numbers), hosted tests that build records by hand

**Interfaces:**
- Consumes: Task 1 (`Observation::clock`).
- Produces (record fields, each defaulted so existing aggregate initializers keep working; add them last):
  - `core::HostedTurn::clock` (`std::optional<double>`), `core::Conversation::clock` (`std::optional<double>`, its start), `core::ConversationEvent::clock` (`std::optional<double>`), `core::Summary::clock` (`std::optional<double>`, a shared copy's), `core::Summary::supersededBy` (`std::int64_t`, 0 for none; replaces `superseded`), `core::Summary::madeIn` (`std::int64_t`, 0 for none), `core::PendingShare::clock` (created), `core::PendingShare::appliedClock` (`std::optional<double>`, null while pending), `core::PendingShare::unappliedBy` (`std::int64_t`).
  - `core::Summary` keeps `bool superseded = false;`, now computed by the readers (superseded by a visible summary), and gains `std::int64_t supersededBy = 0;` and `std::int64_t madeIn = 0;`.
  - `RunStore::SCHEMA_VERSION == 6`.
  - Writers take clocks: `std::string CreateConversation(const std::vector<core::Participant>& participants, std::optional<double> clock)`, `void SetParticipants(std::string_view id, const std::vector<core::Participant>& participants, std::optional<double> clock)`, `void EndConversation(std::string_view id, std::optional<double> clock)`, `void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events, std::optional<double> clock)`; `CommitHostedTurn`, `AddActorSummary`, `AddPendingShare` store the record's clock field.
  - `void MarkShareApplied(std::int64_t id, std::optional<double> clock)` replaces `RemovePendingShare`; `PendingShares()` returns the visible shares that are pending (not applied, or unapplied by the open rollback).
  - `int NextHostedTurn()` (one past every hosted turn ever stored); `LatestHostedTurn()` keeps its name and reads visible turns only. `CommitHostedTurn` requires `record.number == NextHostedTurn()`.
  - `std::optional<double> LatestClock()`: the highest clock among visible hosted turns, conversations, conversation changes, events, summaries and pending shares (created and applied); nullopt when none has one.
  - Every hosted reader returns visible rows only (`hidden_by = 0`): `Conversation`, `Conversations`, `HostedTurns`, `ConversationEvents`, `ActorSummaries`, `HostedSummaries`, `PendingShares`, `RememberedThrough` (marks are not hidden; unchanged), `LatestHostedTurn`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/store/HostedStoreTests.cpp` (it has `HostedMeta()`, `TestHostData()`, `TwoParticipants()`, `TestHostedTurn(number, conversationId)`, `HostedFile(name)`, `Damage(file, sql)`, `ActorSummary(actor, conversation, from, to, text)`; read them first):

```cpp
TEST_CASE("Hosted records keep their clocks, and the run's latest seen clock is the highest", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_clocks") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    CHECK_FALSE(store.LatestClock().has_value());
    const std::string id = store.CreateConversation(TwoParticipants(), 1.0);
    core::HostedTurn turn = TestHostedTurn(1, id);
    turn.clock = 2.5;
    store.CommitHostedTurn(turn);
    store.AddConversationEvents(id, 1, {nlohmann::json{{"text", "A door opens."}}}, 3.0);
    CHECK(store.LatestClock() == 3.0);
    CHECK(store.Conversation(id)->clock == 1.0);
    CHECK(store.HostedTurns(id).at(0).clock == 2.5);
    CHECK(store.ConversationEvents(id).at(0).clock == 3.0);
}

TEST_CASE("A conversation's participants and end are history: the latest change row decides", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_conversation_history") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::vector<core::Participant> two = TwoParticipants();
    const std::string id = store.CreateConversation(two, 1.0);
    std::vector<core::Participant> one = {two.at(0)};
    store.SetParticipants(id, one, 2.0);
    CHECK(store.Conversation(id)->participants.size() == 1);
    store.EndConversation(id, 3.0);
    CHECK(store.Conversation(id)->ended);
    CHECK(store.Conversation(id)->participants.size() == 1);
    // The rows are kept: three changes after the start.
    store::Database db = store::Database::Open(file, false);
    store::Statement count = db.Prepare("SELECT COUNT(*) FROM conversation_changes WHERE conversation_id = ?1");
    count.Bind(1, id);
    REQUIRE(count.Step());
    CHECK(count.ColumnInt(0) == 3);
}

TEST_CASE("Turn numbers continue past hidden turns; the latest turn is the latest visible one", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_next_turn") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), 1.0);
        store.CommitHostedTurn(TestHostedTurn(1, id));
        store.CommitHostedTurn(TestHostedTurn(2, id));
    }
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 1.0, 'abandoned', '{}');"
                 "UPDATE hosted_turns SET hidden_by = 1 WHERE number = 2;");
    store::RunStore store = store::RunStore::Open(file);
    CHECK(store.LatestHostedTurn() == 1);
    CHECK(store.NextHostedTurn() == 3);
    CHECK(store.HostedTurns(id).size() == 1);
    CHECK_THROWS_AS(store.CommitHostedTurn(TestHostedTurn(2, id)), core::ContractError);
    store.CommitHostedTurn(TestHostedTurn(3, id));
}

TEST_CASE("A summary is shown unless a visible condense superseded it", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_superseded_by") / "run.db";
    std::string id;
    std::int64_t condensed = 0;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), 1.0);
        store.CommitHostedTurn(TestHostedTurn(1, id));
        store.FoldConversation(id, ActorSummary("npc-1", id, 1, 1, "First."));
        condensed = store.CondenseActorSummaries("npc-1", ActorSummary("npc-1", "", 1, 1, "Condensed."));
        const std::vector<core::Summary> all = store.ActorSummaries("npc-1", true);
        REQUIRE(all.size() == 2);
        CHECK(all[0].supersededBy == condensed);
        CHECK(all[0].superseded);
        CHECK(store.ActorSummaries("npc-1", false).size() == 1);
    }
    // Hide the condense (as a rollback would): the first summary is shown again.
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 0.5, 'open', '{}');"
                 "UPDATE summaries SET hidden_by = 1 WHERE text = 'Condensed.';");
    store::RunStore store = store::RunStore::Open(file);
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].text == "First.");
    CHECK_FALSE(shown[0].superseded);
}

TEST_CASE("An applied share stays as a row; pending shares are the ones not applied", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_applied_share") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), 1.0);
    core::PendingShare share;
    share.fromActor = "npc-1";
    share.toActor = "npc-2";
    share.conversationId = id;
    share.clock = 1.5;
    const std::int64_t shareId = store.AddPendingShare(share);
    CHECK(store.PendingShares().size() == 1);
    store.MarkShareApplied(shareId, 2.0);
    CHECK(store.PendingShares().empty());
    CHECK(store.LatestClock() == 2.0);
}
```

**Step 1b (the version 5 test, in full).** A version 5 file is easiest to make from SQL: write a helper `V5HostedFile(name)` in the test file that creates the file with `store::Database::Open(file, true)`, executes the version 5 schema (copy `SCHEMA_SQL`, `HOSTED_SCHEMA_SQL` and the per-actor tables exactly as `src/store/RunStore.cpp` has them at HEAD before this task; keep the copy in the test as `V5_SCHEMA_SQL`, the way the file keeps `V2_SCHEMA_SQL`), inserts the run_meta rows (`schema_version` "5", `kind` "hosted", the others as `HostedMeta()`), a host_data row, one conversation (`c1`, participants as `TwoParticipants()` serialized as RunStore stores them, ended 1, folded_through 1), one hosted turn (copy the column values `TestHostedTurn(1, "c1")` would store), and two summaries of `npc-1` (`First.`, `superseded = 1`; then `Condensed.` with conversation_id '' ). Then:

```cpp
TEST_CASE("A version 5 hosted run is upgraded to version 6: conversations become history, superseded summaries name "
          "their condense",
          "[store][hosted][rollback]")
{
    const std::filesystem::path file = V5HostedFile("store_upgrade_v5");
    store::RunStore store = store::RunStore::Open(file);
    const std::optional<core::Conversation> conversation = store.Conversation("c1");
    REQUIRE(conversation);
    CHECK(conversation->ended);
    CHECK(conversation->participants.size() == 2);
    CHECK_FALSE(conversation->clock.has_value());
    const std::vector<core::Summary> all = store.ActorSummaries("npc-1", true);
    REQUIRE(all.size() == 2);
    CHECK(all[0].supersededBy == all[1].id);
    CHECK(store.ActorSummaries("npc-1", false).size() == 1);
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "6");
}
```

Remove the placeholder body of the earlier version 5 test case (it is replaced by this one).

Update every call of the changed writers in tests (`grep -rn "CreateConversation(\|SetParticipants(\|EndConversation(\|AddConversationEvents(\|RemovePendingShare(" tests src`): tests pass `std::nullopt` (or a clock where the test is about clocks). Update the expected schema version to 6 where tests check it.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (the new parameters and fields).

- [ ] **Step 3: The records**

Add the fields listed under **Produces**, each last in its struct with a one-line comment, `#include <optional>` where needed. `core::Summary` keeps `bool superseded = false;` (now computed by the readers) and gains `std::int64_t supersededBy = 0; // The condensed summary that replaced it; 0 for none.` and `std::int64_t madeIn = 0; // The rollback it was written in, while open; 0 otherwise.`

- [ ] **Step 4: The schema and the upgrade**

In `src/store/RunStore.cpp`:

- New files: the hosted tables gain the version 6 columns; `conversations` loses `participants` and `ended`; new tables:

```sql
CREATE TABLE conversation_changes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id TEXT NOT NULL,
    clock           REAL,
    participants    TEXT NOT NULL,
    ended           INTEGER NOT NULL,
    hidden_by       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX conversation_changes_conversation ON conversation_changes (conversation_id);
CREATE TABLE rollbacks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    game_time   REAL NOT NULL,
    status      TEXT NOT NULL,
    saved_marks TEXT NOT NULL
);
```

  and the columns: `hosted_turns`, `conversation_events`, `conversations`: `clock REAL`, `hidden_by INTEGER NOT NULL DEFAULT 0`; `summaries`: `clock REAL`, `hidden_by INTEGER NOT NULL DEFAULT 0`, `superseded_by INTEGER NOT NULL DEFAULT 0`, `made_in INTEGER NOT NULL DEFAULT 0` (and no `superseded` column); `pending_shares`: `clock REAL`, `applied_clock REAL`, `unapplied_by INTEGER NOT NULL DEFAULT 0`, `hidden_by INTEGER NOT NULL DEFAULT 0`. Keep the schema texts so that a new file and an upgraded one end with the same tables and columns (a test can compare `PRAGMA table_info` of both).

- The upgrade chain gains version 5 to 6, in the same transaction as the earlier steps:

```cpp
    // Version 6 (rollback spec, section 4): clocks and hidden marks on hosted rows, conversations as history, and a
    // superseded summary naming its condense. Existing rows get no clock: a rewind never hides them.
    auto upgradeTo6 = [&db, &file] {
        db.Exec(V6_COLUMNS_SQL); // ALTER TABLE ... ADD COLUMN for every new column above.
        db.Exec(V6_TABLES_SQL);  // conversation_changes, its index, rollbacks.
        // Each conversation's participants and end become its first change row.
        db.Exec("INSERT INTO conversation_changes (conversation_id, clock, participants, ended) "
                "SELECT id, NULL, participants, ended FROM conversations ORDER BY position");
        // A superseded summary names its condense: the actor's first condensed summary written after it (a
        // condensed summary has no conversation and no teller).
        Statement superseded = db.Prepare("SELECT id, actor_id FROM summaries WHERE superseded = 1 ORDER BY id");
        std::vector<std::pair<std::int64_t, std::string>> rows;
        while (superseded.Step()) {
            rows.emplace_back(superseded.ColumnInt(0), superseded.ColumnText(1));
        }
        for (const auto& [id, actor] : rows) {
            Statement condense = db.Prepare("SELECT id FROM summaries WHERE actor_id = ?1 AND id > ?2 AND "
                                            "conversation_id = '' AND told_by = '' ORDER BY id LIMIT 1");
            condense.Bind(1, actor).Bind(2, id);
            if (!condense.Step()) {
                throw StoreError(std::format("'{}': summary {}: superseded, but no later condensed summary of '{}'",
                                             core::PathText(file), id, actor));
            }
            db.Prepare("UPDATE summaries SET superseded_by = ?1 WHERE id = ?2").Bind(1, condense.ColumnInt(0)).Bind(2, id).Run();
        }
        db.Exec("ALTER TABLE summaries DROP COLUMN superseded;"
                "ALTER TABLE conversations DROP COLUMN participants;"
                "ALTER TABLE conversations DROP COLUMN ended;");
    };
```

  (Drop an index on a column before dropping the column, if SQLite refuses; `summaries_actor` is on `actor_id` and stays. Owned runs run the same SQL: their hosted tables exist, empty.)

- [ ] **Step 5: Writers and readers**

- The writers take and store clocks as listed. `CreateConversation` inserts the conversation row (`clock`) and its first change row (participants, `ended` 0, same clock), in one transaction. `SetParticipants` appends a change row with the new participants and the current `ended`; `EndConversation` appends one with the current participants and `ended` 1. `AddActorSummary`, `RememberFold`, `FoldConversation`, `CondenseActorSummaries` store `summary.clock` and stamp `made_in` with the open rollback's id when there is one (a private `OpenRollbackId()` read inside the same transaction; Task 3 adds the public `OpenRollback()`; 0 when none). `CondenseActorSummaries` inserts the condensed summary first, then sets `superseded_by` = its id on the actor's shown summaries.
- `MarkShareApplied(id, clock)`: `UPDATE pending_shares SET applied_clock = ?2, unapplied_by = 0 WHERE id = ?1`.
- Readers: every hosted reader adds `hidden_by = 0` for its table. `Conversation`/`Conversations` read the conversation rows, then each one's latest visible change row (`ORDER BY id DESC LIMIT 1`, `hidden_by = 0`) for its participants and ended flag (a conversation with no visible change row is damaged: StoreError naming the file and `conversation_changes`). `ActorSummaries(actor, includeSuperseded)`: `hidden_by = 0`, and computes `superseded` as "superseded by a visible summary":

```sql
SELECT s.id, s.from_turn, s.to_turn, s.text, s.actor_id, s.conversation_id, s.told_by, s.source_id, s.superseded_by,
       s.made_in, s.clock,
       EXISTS (SELECT 1 FROM summaries c WHERE c.id = s.superseded_by AND c.hidden_by = 0) AS superseded
FROM summaries s WHERE s.actor_id = ?1 AND s.hidden_by = 0
```

  plus `AND NOT superseded` when `includeSuperseded` is false; `ORDER BY s.from_turn, s.id`. `ReadSummaryRow` reads the new columns. `PendingShares()`: `hidden_by = 0 AND (applied_clock IS NULL OR unapplied_by <> 0)`. `LatestHostedTurn()`: `MAX(number) WHERE hidden_by = 0`; `NextHostedTurn()`: `MAX(number) + 1` over all rows (1 for none).
- `LatestClock()`:

```cpp
std::optional<double> RunStore::LatestClock()
{
    RequireHosted("LatestClock");
    // The highest clock among visible records: what a request with no observation of its own is dated with.
    Statement query = _db.Prepare(
        "SELECT MAX(c) FROM ("
        " SELECT clock AS c FROM hosted_turns WHERE hidden_by = 0 UNION ALL"
        " SELECT clock FROM conversations WHERE hidden_by = 0 UNION ALL"
        " SELECT clock FROM conversation_changes WHERE hidden_by = 0 UNION ALL"
        " SELECT clock FROM conversation_events WHERE hidden_by = 0 UNION ALL"
        " SELECT clock FROM summaries WHERE hidden_by = 0 UNION ALL"
        " SELECT clock FROM pending_shares WHERE hidden_by = 0 UNION ALL"
        " SELECT applied_clock FROM pending_shares WHERE hidden_by = 0 AND unapplied_by = 0)");
    if (!query.Step() || query.ColumnIsNull(0)) {
        return std::nullopt;
    }
    return query.ColumnDouble(0);
}
```

  (Add to `store::Statement` in `src/store/Sqlite.h/.cpp`, with a test in the store tests: `bool ColumnIsNull(int index) const`, `double ColumnDouble(int index) const`, `Statement& Bind(int index, double value)` and `Statement& Bind(int index, std::optional<double> value)` (NULL for nullopt). The plain `double` overload is required: without it a `double` argument converts to the `std::int64_t` overload, a standard conversion that beats the user-defined one to `std::optional<double>`, and the clock is truncated.)

- [ ] **Step 6: The session passes clocks**

In `src/turn/HostedSession.cpp`: `StartConversation` passes `observed.clock`; `SetParticipants` passes the observation's clock; `EndConversation`, `RecordEvents`, `ApplyShare`'s copies (`copy.clock`), `ShareMemories`' pending row (`share.clock`) and `Maintain`'s `MarkShareApplied` use `_store.LatestClock()`; `RunTurn` sets `record.clock = observation.clock` and numbers the turn `_store.NextHostedTurn()`; replace `RemovePendingShare` with `MarkShareApplied`. No behaviour changes otherwise: every hosted test passes as before.

- [ ] **Step 7: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`, `"[hosted]"`, `"[memory]"`, `"[hosted-server]"`, `"[turn]"`, `"[replay]"`, `"[context]"`, then `ctest --preset tests`.
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add src/core/ src/store/ src/turn/HostedSession.cpp tests/
git commit -m "Store version 6: clocks on hosted records, conversations as history, superseded summaries name their condense, visible-only readers"
```

---


