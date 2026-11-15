### Task 1: Store version 4: each hosted turn keeps how many events its prompt left out

**Files:**
- Modify: `src/core/HostedRecord.h` (struct `HostedTurn`)
- Modify: `src/store/RunStore.h:54` (`SCHEMA_VERSION`)
- Modify: `src/store/RunStore.cpp` (`HOSTED_SCHEMA_SQL`, the upgrade in `RunStore::Open`, `CommitHostedTurn`, `HostedTurns`)
- Test: `tests/store/HostedStoreTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int core::HostedTurn::eventsOmitted = 0;` stored in `hosted_turns.events_omitted` and read back by `RunStore::HostedTurns`. `store::RunStore::SCHEMA_VERSION == 4`.

- [ ] **Step 1: Write the failing tests**

In `tests/store/HostedStoreTests.cpp`, rename the existing upgrade test and update its version check:

```cpp
TEST_CASE("A version 2 run is upgraded in place to an owned version 4 run, its data unchanged", "[store][hosted]")
```

and in its body:

```cpp
    // The file now says version 4 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "4");
```

(the rest of that block is unchanged). Then append these test cases at the end of the file. They use the file's existing helpers `HostedMeta()`, `TestHostData()`, `TwoParticipants()`, `TestHostedTurn(number, conversationId)`, `HostedFile(name)` (a hosted run with conversation c1, one turn and one event) and `Damage(file, sql)`:

```cpp
TEST_CASE("A hosted turn keeps how many events its prompt left out", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_events_omitted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants());
        core::HostedTurn turn = TestHostedTurn(1, id);
        turn.eventsOmitted = 3;
        store.CommitHostedTurn(turn);
        store.CommitHostedTurn(TestHostedTurn(2, id));
    }
    store::RunStore reopened = store::RunStore::Open(file);
    const std::vector<core::HostedTurn> turns = reopened.HostedTurns(id);
    REQUIRE(turns.size() == 2);
    CHECK(turns[0].eventsOmitted == 3);
    CHECK(turns[1].eventsOmitted == 0);
}

TEST_CASE("A version 3 run is upgraded in place to version 4, its turns with no events left out", "[store][hosted]")
{
    // A version 3 file as RunStore wrote it: a version 4 file without the new column, marked version 3.
    const std::filesystem::path file = HostedFile("store_upgrade_v3");
    Damage(file, "ALTER TABLE hosted_turns DROP COLUMN events_omitted;"
                 "UPDATE run_meta SET value = '3' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        const std::vector<core::HostedTurn> turns = store.HostedTurns("c1");
        REQUIRE(turns.size() == 1);
        CHECK(turns[0].eventsOmitted == 0);
        CHECK(store.ConversationEvents("c1").size() == 1);
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "4");
}

TEST_CASE("A failed upgrade from version 3 names the file and leaves it a version 3 run", "[store][hosted]")
{
    // The column already exists, so adding it fails and the transaction rolls back.
    const std::filesystem::path file = HostedFile("store_upgrade_v3_fails");
    Damage(file, "UPDATE run_meta SET value = '3' WHERE key = 'schema_version'");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("version 3"));
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "3");
}

TEST_CASE("A negative events_omitted is a StoreError naming the file and the column", "[store][hosted]")
{
    const std::filesystem::path file = HostedFile("store_damaged_omitted");
    Damage(file, "UPDATE hosted_turns SET events_omitted = -1");
    store::RunStore store = store::RunStore::Open(file);
    CHECK_THROWS_AS(store.HostedTurns("c1"), store::StoreError);
    CHECK_THROWS_WITH(store.HostedTurns("c1"), ContainsSubstring("run.db") && ContainsSubstring("events_omitted"));
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: a compile error, `'eventsOmitted': is not a member of 'morpheus::core::HostedTurn'`.

- [ ] **Step 3: Add the field**

In `src/core/HostedRecord.h`, in `struct HostedTurn`, after `std::string line = {};`:

```cpp
    // How many of the events before the turn (recorded since the conversation's previous turn, then the request's)
    // its prompt left out, the oldest first, because the tail was over tail_allowance. The store keeps them all.
    int eventsOmitted = 0;
```

- [ ] **Step 4: Store it: schema version 4 and its upgrades**

In `src/store/RunStore.h`, change `static constexpr int SCHEMA_VERSION = 3;` to `static constexpr int SCHEMA_VERSION = 4;`.

In `src/store/RunStore.cpp`, replace the comment above `HOSTED_SCHEMA_SQL` and the `hosted_turns` table in it:

```cpp
// The tables version 3 adds, with version 4's events_omitted: a hosted run's host data, its conversations, its turns
// and the events recorded between them. Created in every file, so a version 2 file is upgraded by adding them.
constexpr const char* HOSTED_SCHEMA_SQL = R"sql(
```

```sql
CREATE TABLE hosted_turns (
    number          INTEGER PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    speaker_id      TEXT NOT NULL,
    listener_ids    TEXT NOT NULL,
    player_text     TEXT NOT NULL,
    events          TEXT NOT NULL,
    observation     TEXT NOT NULL,
    resolve_output  TEXT NOT NULL,
    adjudication    TEXT NOT NULL,
    line            TEXT NOT NULL,
    timings         TEXT NOT NULL,
    events_omitted  INTEGER NOT NULL DEFAULT 0
);
```

Right after `HOSTED_SCHEMA_SQL`, add:

```cpp
// What version 4 adds to a version 3 file: each hosted turn's count of the events its prompt left out, 0 for the
// turns already played.
constexpr const char* EVENTS_OMITTED_SQL =
    "ALTER TABLE hosted_turns ADD COLUMN events_omitted INTEGER NOT NULL DEFAULT 0;";
```

In `RunStore::Open`, replace the whole `if (value("schema_version") == "2") { ... }` block with:

```cpp
    // An older run is upgraded in place, in one transaction, its data untouched: version 2 gains the hosted tables
    // (already with version 4's column) and is marked owned; version 3 gains hosted_turns.events_omitted, 0 for the
    // turns it holds. Any other version is refused below.
    const std::string version = value("schema_version"); // A copy: `values` changes below.
    if (version == "2" || version == "3") {
        try {
            db.Transaction([&] {
                if (version == "2") {
                    db.Exec(HOSTED_SCHEMA_SQL);
                    db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)")
                        .Bind(1, RunKindName(RunKind::Owned))
                        .Run();
                } else {
                    db.Exec(EVENTS_OMITTED_SQL);
                }
                db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'schema_version'")
                    .Bind(1, std::to_string(SCHEMA_VERSION))
                    .Run();
            });
        } catch (const StoreError& error) {
            // The transaction rolled back: the file is still at its old version.
            throw StoreError(std::format("'{}': cannot upgrade from schema version {} to {}: {}",
                                         core::PathText(file), version, SCHEMA_VERSION, error.what()));
        }
        values["schema_version"] = std::to_string(SCHEMA_VERSION);
        if (version == "2") {
            values["kind"] = RunKindName(RunKind::Owned);
        }
    }
```

In `RunStore::CommitHostedTurn`, add next to its other `MORPHEUS_REQUIRE` checks (before the insert):

```cpp
    MORPHEUS_REQUIRE(record.eventsOmitted >= 0, "RunStore::CommitHostedTurn: eventsOmitted must not be negative");
```

and replace the insert with:

```cpp
    _db.Prepare("INSERT INTO hosted_turns (number, conversation_id, speaker_id, listener_ids, player_text, events, "
                "observation, resolve_output, adjudication, line, timings, events_omitted) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)")
        .Bind(1, record.number)
        .Bind(2, record.conversationId)
        .Bind(3, record.speakerId)
        .Bind(4, DumpText(listeners))
        .Bind(5, DumpText(playerText))
        .Bind(6, DumpText(events))
        .Bind(7, DumpText(record.observation))
        .Bind(8, record.resolveOutput)
        .Bind(9, DumpText(adjudication))
        .Bind(10, record.line)
        .Bind(11, DumpText(record.timings))
        .Bind(12, record.eventsOmitted)
        .Run();
```

In `RunStore::HostedTurns`, select the column last:

```cpp
    Statement query = _db.Prepare("SELECT number, speaker_id, listener_ids, player_text, events, observation, "
                                  "resolve_output, adjudication, line, timings, events_omitted FROM hosted_turns "
                                  "WHERE conversation_id = ?1 ORDER BY number");
```

and after `turn.timings = parseColumn(9, "timings");`:

```cpp
        turn.eventsOmitted = static_cast<int>(query.ColumnInt(10));
        if (turn.eventsOmitted < 0) {
            throw damaged("events_omitted", "is negative");
        }
```

(`damaged` is the lambda `HostedTurns` already uses for its other columns. If `ColumnInt` returns `std::int64_t`, the `static_cast<int>` above is the conversion; keep it explicit.)

- [ ] **Step 5: Find every other place that names version 3**

Run: `grep -rn "version 3\|SCHEMA_VERSION\|\"3\"" src tests | grep -iv "api_version"`
Expected: only the places this task already changed, or tests that build version 2 files (leave those). Update any other test that checks the stored version is 3 so it expects 4, and any comment that calls the current version 3 (`src/store/RunStore.h`'s class comment, if it says so).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: `All tests passed`.

Then: `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: `All tests passed` (hosted sessions write and read the new column).

- [ ] **Step 7: Commit**

```bash
git add src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp
git commit -m "Store version 4: a hosted turn keeps how many events its prompt left out; version 3 and 2 files upgrade in place"
```

(Add the implementing model's `Co-Authored-By` trailer as the message's last line.)

---

