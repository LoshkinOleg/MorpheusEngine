### Task 1: Store version 5: summaries per actor, remembered-through marks, pending shares

**Files:**
- Modify: `src/core/TurnRecord.h` (struct `Summary`), `src/core/HostedRecord.h` (new `PendingShare`)
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Test: `tests/store/HostedStoreTests.cpp`, `tests/store/RunStoreTests.cpp` (version number checks only)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `core::Summary` fields after `text`: `std::string actorId = {}; std::string conversationId = {}; std::string toldBy = {}; std::int64_t sourceId = 0; bool superseded = false; std::int64_t id = 0;`
  - `struct core::PendingShare { std::int64_t id = 0; std::string fromActor = {}; std::string toActor = {}; std::string conversationId = {}; };`
  - `store::EmbeddingKind::Summary` (stored as `"summary"`, refId the summary's id in decimal).
  - `RunStore::SCHEMA_VERSION == 5`.
  - Hosted-only methods (ContractError on an owned run): `std::vector<core::Summary> ActorSummaries(std::string_view actorId, bool includeSuperseded)`, `std::vector<core::Summary> HostedSummaries()`, `std::int64_t AddActorSummary(const core::Summary& summary)`, `std::int64_t RememberFold(const core::Summary& summary)`, `std::int64_t CondenseActorSummaries(std::string_view actorId, const core::Summary& condensed)`, `int RememberedThrough(std::string_view conversationId, std::string_view actorId)`, `void SetRememberedThrough(std::string_view conversationId, std::string_view actorId, int turn)`, `void MarkFolded(std::string_view conversationId, int turn)`, `std::int64_t AddPendingShare(const core::PendingShare& share)`, `std::vector<core::PendingShare> PendingShares()`, `void RemovePendingShare(std::int64_t id)`.
  - `FoldConversation(std::string_view id, const core::Summary& summary)` now returns `std::int64_t` (the new summary's id), requires a non-empty `summary.actorId` and `summary.conversationId == id`, and also moves that actor's remembered-through mark to `summary.toTurn`.
  - `AddSummary`, `ReplaceSummaries` and `Summaries` are owned-only (ContractError on a hosted run) and see only rows with an empty actor.

- [ ] **Step 1: Write the failing tests**

Append to `tests/store/HostedStoreTests.cpp` (it has `HostedMeta()`, `TestHostData()`, `TwoParticipants()` (Hulda 0001A6A4 and Aela 00000007, the player), `TestHostedTurn(number, conversationId)`, `HostedFile(name)`, `Damage(file, sql)`; read them first, and `TestHostedTurn`'s speaker and listeners):

```cpp
namespace {

// An NPC summary of conversation `conversationId` by `actorId` over turns `from`..`to`.
core::Summary ActorSummary(const std::string& actorId, const std::string& conversationId, int from, int to,
                           const std::string& text)
{
    core::Summary summary;
    summary.fromTurn = from;
    summary.toTurn = to;
    summary.text = text;
    summary.actorId = actorId;
    summary.conversationId = conversationId;
    return summary;
}

} // namespace

TEST_CASE("A hosted run keeps summaries per actor, with their conversation, teller and supersede mark",
          "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_actor_summaries") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    // A fold by the speaker moves the conversation's mark and the speaker's own.
    const std::int64_t first = store.FoldConversation(id, ActorSummary("0001A6A4", id, 1, 1, "Hulda greeted Aela."));
    CHECK(first > 0);
    CHECK(store.Conversation(id)->foldedThrough == 1);
    CHECK(store.RememberedThrough(id, "0001A6A4") == 1);
    CHECK(store.RememberedThrough(id, "0001B07C") == 0); // No row: 0.

    // A catch-up fold moves only the actor's mark.
    store.RememberFold(ActorSummary("0001B07C", id, 1, 1, "Saadia saw Hulda greet Aela."));
    CHECK(store.RememberedThrough(id, "0001B07C") == 1);
    CHECK(store.Conversation(id)->foldedThrough == 1);

    // A shared copy.
    core::Summary copy = ActorSummary("0001B07C", id, 1, 1, "Hulda greeted Aela.");
    copy.toldBy = "0001A6A4";
    copy.sourceId = first;
    const std::int64_t copied = store.AddActorSummary(copy);

    const std::vector<core::Summary> saadia = store.ActorSummaries("0001B07C", false);
    REQUIRE(saadia.size() == 2);
    CHECK(saadia[0].text == "Saadia saw Hulda greet Aela.");
    CHECK(saadia[1].id == copied);
    CHECK(saadia[1].toldBy == "0001A6A4");
    CHECK(saadia[1].sourceId == first);
    CHECK(saadia[1].conversationId == id);

    // Condensing supersedes the shown ones and adds one with no conversation and no teller.
    core::Summary condensed = ActorSummary("0001B07C", "", 1, 1, "Saadia remembers the greeting.");
    const std::int64_t condensedId = store.CondenseActorSummaries("0001B07C", condensed);
    const std::vector<core::Summary> shown = store.ActorSummaries("0001B07C", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == condensedId);
    CHECK(shown[0].conversationId.empty());
    const std::vector<core::Summary> all = store.ActorSummaries("0001B07C", true);
    REQUIRE(all.size() == 3);
    CHECK(all[0].superseded);
    CHECK(all[1].superseded);
    CHECK_FALSE(all[2].superseded);
    CHECK(store.HostedSummaries().size() == 4); // Hulda's one and Saadia's three.

    // The marks only move forward.
    store.SetRememberedThrough(id, "0001B07C", 2);
    CHECK(store.RememberedThrough(id, "0001B07C") == 2);
    CHECK_THROWS_AS(store.SetRememberedThrough(id, "0001B07C", 1), core::ContractError);
    store.MarkFolded(id, 2);
    CHECK(store.Conversation(id)->foldedThrough == 2);
}

TEST_CASE("A hosted run keeps pending shares until they are removed", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_pending_shares") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    const std::int64_t share = store.AddPendingShare({0, "0001A6A4", "0001B07C", id});
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == share);
    CHECK(pending[0].fromActor == "0001A6A4");
    CHECK(pending[0].toActor == "0001B07C");
    CHECK(pending[0].conversationId == id);
    store.RemovePendingShare(share);
    CHECK(store.PendingShares().empty());
}

TEST_CASE("Run-wide summary methods are the owned game's; per-actor folds need an actor", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_summary_guards") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    store.CommitHostedTurn(TestHostedTurn(1, id));
    CHECK_THROWS_AS(store.Summaries(), core::ContractError);
    CHECK_THROWS_AS(store.AddSummary({1, 1, "x"}), core::ContractError);
    CHECK_THROWS_AS(store.FoldConversation(id, {1, 1, "x"}), core::ContractError); // No actor.
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("0001A6A4", "c9", 1, 1, "x")), core::ContractError);
}

TEST_CASE("A version 4 hosted run is upgraded to version 5: each summary goes to the NPCs who witnessed it",
          "[store][hosted][memory]")
{
    // A version 4 file as RunStore wrote it: a version 5 file without the new columns and tables, marked version 4,
    // with one run-wide summary of turn 1 (Hulda spoke to Aela, the player).
    const std::filesystem::path file = HostedFile("store_upgrade_v4");
    Damage(file, "UPDATE conversations SET folded_through = 1;"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'Hulda greeted Aela.');"
                 "DROP TABLE actor_memory; DROP TABLE pending_shares;"
                 "ALTER TABLE summaries DROP COLUMN actor_id; ALTER TABLE summaries DROP COLUMN conversation_id;"
                 "ALTER TABLE summaries DROP COLUMN told_by; ALTER TABLE summaries DROP COLUMN source_id;"
                 "ALTER TABLE summaries DROP COLUMN superseded;"
                 "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        // Hulda, the NPC speaker, has it; Aela, the player, has none.
        const std::vector<core::Summary> hulda = store.ActorSummaries("0001A6A4", true);
        REQUIRE(hulda.size() == 1);
        CHECK(hulda[0].text == "Hulda greeted Aela.");
        CHECK(hulda[0].conversationId == "c1");
        CHECK(store.ActorSummaries("00000007", true).empty());
        CHECK(store.RememberedThrough("c1", "0001A6A4") == 1);
        CHECK(store.HostedSummaries().size() == 1); // The run-wide row is gone.
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "5");
}

TEST_CASE("A hosted summary row without its actor is a StoreError naming the file and the column",
          "[store][hosted][memory]")
{
    const std::filesystem::path file = HostedFile("store_damaged_actor");
    Damage(file, "INSERT INTO summaries (from_turn, to_turn, text, actor_id) VALUES (1, 1, 'x', '');");
    store::RunStore store = store::RunStore::Open(file);
    CHECK_THROWS_AS(store.HostedSummaries(), store::StoreError);
    CHECK_THROWS_WITH(store.HostedSummaries(), ContainsSubstring("run.db") && ContainsSubstring("actor_id"));
}
```

(`HostedFile` must commit turn 1 with Hulda as speaker and Aela as a listener; if its `TestHostedTurn` differs, adapt the expected actors in the upgrade test to what it stores, keeping the rule: NPC witnesses get the summary, the player does not.)

Also add an owned-run upgrade test next to the existing version 2 one: a version 4 owned file (make it as the version 3 test does: create, then drop the new columns and tables and mark it version 4) opens as version 5, and its `Summaries()` are unchanged.

In the existing upgrade tests, change every expected `"4"` (the current version) to `"5"`, and in `tests/store/RunStoreTests.cpp` every `SCHEMA_VERSION == 4` or "supported version 4" to 5.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`'actorId': is not a member of 'morpheus::core::Summary'`).

- [ ] **Step 3: The record types**

In `src/core/TurnRecord.h`, `struct Summary`, after `std::string text = {};` (keep the first three fields first: code builds summaries with `{from, to, text}`):

```cpp
    // Hosted runs (per-speaker memory spec): whose memory this is, and from which conversation. The owned game leaves
    // them empty. A condensed summary has no conversation; a shared copy names its teller and the summary it copies.
    std::string actorId = {};
    std::string conversationId = {};
    std::string toldBy = {};
    std::int64_t sourceId = 0; // The summary a shared copy was made from; 0 otherwise.
    bool superseded = false;   // Condensed into a later summary: kept, but no longer shown.
    std::int64_t id = 0;       // The store's row id; 0 before it is stored.
```

(add `#include <cstdint>`). In `src/core/HostedRecord.h`, after `ConversationEvent`:

```cpp
// A share of one NPC's memories of a conversation with another, waiting until the teller's memory of it is complete
// (per-speaker memory spec, section 2).
struct PendingShare {
    std::int64_t id = 0;
    std::string fromActor = {};
    std::string toActor = {};
    std::string conversationId = {};
};
```

- [ ] **Step 4: The schema and the upgrade**

In `src/store/RunStore.h`: `SCHEMA_VERSION = 5`; `enum class EmbeddingKind { Lore, Turn, Note, Summary };` with the refId comment extended ("decimal summary id for Summary").

In `src/store/RunStore.cpp`:

- `KindName` returns `"summary"` for `EmbeddingKind::Summary` (and the kind parser, if there is one, reads it back).
- The `summaries` table in `SCHEMA_SQL` gains, after `text`:

```sql
    actor_id        TEXT NOT NULL DEFAULT '',
    conversation_id TEXT NOT NULL DEFAULT '',
    told_by         TEXT NOT NULL DEFAULT '',
    source_id       INTEGER NOT NULL DEFAULT 0,
    superseded      INTEGER NOT NULL DEFAULT 0
```

  and `HOSTED_SCHEMA_SQL` gains:

```sql
CREATE TABLE actor_memory (
    conversation_id    TEXT NOT NULL,
    actor_id           TEXT NOT NULL,
    remembered_through INTEGER NOT NULL,
    PRIMARY KEY (conversation_id, actor_id)
);
CREATE TABLE pending_shares (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    from_actor      TEXT NOT NULL,
    to_actor        TEXT NOT NULL,
    conversation_id TEXT NOT NULL
);
CREATE INDEX summaries_actor ON summaries (actor_id);
```

  (Both `SCHEMA_SQL` and `HOSTED_SCHEMA_SQL` run for every new file; a version 2 file runs `HOSTED_SCHEMA_SQL` in its upgrade, so give version 2 files the new summary columns too: add them through the same `ALTER TABLE` list as version 4 below.)

- A constant for what version 5 adds to a version 4 file:

```cpp
// What version 5 adds to a version 4 file: the summaries' per-actor columns and the per-speaker memory tables
// (per-speaker memory spec, section 5). A hosted run's run-wide summaries are then moved to their NPC witnesses.
constexpr const char* PER_ACTOR_SQL = R"sql(
ALTER TABLE summaries ADD COLUMN actor_id TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN conversation_id TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN told_by TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN source_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE summaries ADD COLUMN superseded INTEGER NOT NULL DEFAULT 0;
CREATE TABLE actor_memory (
    conversation_id    TEXT NOT NULL,
    actor_id           TEXT NOT NULL,
    remembered_through INTEGER NOT NULL,
    PRIMARY KEY (conversation_id, actor_id)
);
CREATE TABLE pending_shares (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    from_actor      TEXT NOT NULL,
    to_actor        TEXT NOT NULL,
    conversation_id TEXT NOT NULL
);
CREATE INDEX summaries_actor ON summaries (actor_id);
)sql";
```

  To avoid writing the two new tables twice, remove them from `HOSTED_SCHEMA_SQL` and run `PER_ACTOR_SQL`'s table and index statements for new files by splitting it into `PER_ACTOR_COLUMNS_SQL` (the `ALTER TABLE` lines, for upgrades only) and `PER_ACTOR_TABLES_SQL` (the tables and the index, for new files and upgrades); new files get the columns from `SCHEMA_SQL`.

- `RunStore::Open`'s upgrade becomes a chain in one transaction: version 2 runs `HOSTED_SCHEMA_SQL`, inserts `kind = owned`, then everything version 3 and 4 need; version 3 runs `EVENTS_OMITTED_SQL`, then what version 4 needs; version 4 runs `PER_ACTOR_COLUMNS_SQL` and `PER_ACTOR_TABLES_SQL`, then, for a hosted run, moves its summaries (below). Then the version is set to 5. Keep the error message "cannot upgrade from schema version {old} to 5".

  Moving a version 4 hosted run's summaries, inside the same transaction (read the kind from `values["kind"]` first; a version 2 file is owned):

```cpp
    // Each run-wide summary goes to every NPC who witnessed a turn in its range, with that turn's conversation; the
    // run-wide row goes. Each NPC witness's mark in a conversation starts at the conversation's folded_through.
    auto moveSummaries = [&db, &file] {
        struct Witnessed {
            std::string conversationId = {};
            std::vector<std::string> npcs = {};
        };
        // Every turn's conversation and NPC witnesses (the speaker and the listeners who are not the player).
        std::map<int, Witnessed> turns;
        Statement turnQuery = db.Prepare("SELECT number, conversation_id, speaker_id, listener_ids, observation "
                                         "FROM hosted_turns ORDER BY number");
        while (turnQuery.Step()) {
            const int number = static_cast<int>(turnQuery.ColumnInt(0));
            const nlohmann::json listeners = nlohmann::json::parse(turnQuery.ColumnText(3), nullptr, false);
            const nlohmann::json observation = nlohmann::json::parse(turnQuery.ColumnText(4), nullptr, false);
            const core::ActorsCore core = core::ReadActors(observation);
            if (listeners.is_discarded() || !listeners.is_array() || !core.faults.empty()) {
                throw StoreError(std::format("'{}': hosted turn {} cannot be upgraded: its listener_ids or "
                                             "observation is damaged",
                                             core::PathText(file), number));
            }
            auto isPlayer = [&core](const std::string& id) {
                return std::any_of(core.actors.begin(), core.actors.end(),
                                   [&id](const core::Actor& actor) { return actor.id == id && actor.isPlayer; });
            };
            Witnessed witnessed;
            witnessed.conversationId = turnQuery.ColumnText(1);
            witnessed.npcs.push_back(turnQuery.ColumnText(2));
            for (const nlohmann::json& listener : listeners) {
                if (listener.is_string() && !isPlayer(listener.get<std::string>())) {
                    witnessed.npcs.push_back(listener.get<std::string>());
                }
            }
            turns.emplace(number, std::move(witnessed));
        }
        // Each run-wide summary, copied to the NPC witnesses of its range.
        Statement summaryQuery = db.Prepare("SELECT id, from_turn, to_turn, text FROM summaries ORDER BY id");
        std::vector<std::int64_t> moved;
        while (summaryQuery.Step()) {
            const int from = static_cast<int>(summaryQuery.ColumnInt(1));
            const int to = static_cast<int>(summaryQuery.ColumnInt(2));
            std::set<std::string> npcs;
            std::string conversationId;
            for (auto it = turns.lower_bound(from); it != turns.end() && it->first <= to; ++it) {
                conversationId = it->second.conversationId;
                npcs.insert(it->second.npcs.begin(), it->second.npcs.end());
            }
            for (const std::string& npc : npcs) {
                db.Prepare("INSERT INTO summaries (from_turn, to_turn, text, actor_id, conversation_id) "
                           "VALUES (?1, ?2, ?3, ?4, ?5)")
                    .Bind(1, from)
                    .Bind(2, to)
                    .Bind(3, summaryQuery.ColumnText(3))
                    .Bind(4, npc)
                    .Bind(5, conversationId)
                    .Run();
            }
            moved.push_back(summaryQuery.ColumnInt(0));
        }
        for (const std::int64_t id : moved) {
            db.Prepare("DELETE FROM summaries WHERE id = ?1").Bind(1, id).Run();
        }
        // Each NPC witness's mark: how far its conversation is folded.
        Statement conversationQuery = db.Prepare("SELECT id, folded_through FROM conversations");
        while (conversationQuery.Step()) {
            const std::string conversationId = conversationQuery.ColumnText(0);
            const int foldedThrough = static_cast<int>(conversationQuery.ColumnInt(1));
            std::set<std::string> npcs;
            for (const auto& [number, witnessed] : turns) {
                if (witnessed.conversationId == conversationId && number <= foldedThrough) {
                    npcs.insert(witnessed.npcs.begin(), witnessed.npcs.end());
                }
            }
            for (const std::string& npc : npcs) {
                db.Prepare("INSERT INTO actor_memory (conversation_id, actor_id, remembered_through) VALUES (?1, ?2, ?3)")
                    .Bind(1, conversationId)
                    .Bind(2, npc)
                    .Bind(3, foldedThrough)
                    .Run();
            }
        }
    };
```

  (A statement read while inserting into the same table: collect the rows first if SQLite's cursor misbehaves; the code above inserts while `summaryQuery` steps over `summaries`. Read all summary rows into a vector before inserting, to be safe.)

- [ ] **Step 5: The methods**

Declare in `RunStore.h` (with comments) the methods listed under **Produces**, and implement them in `RunStore.cpp`:

```cpp
std::vector<core::Summary> RunStore::ActorSummaries(std::string_view actorId, bool includeSuperseded)
{
    RequireHosted("ActorSummaries");
    MORPHEUS_REQUIRE(!actorId.empty(), "ActorSummaries: an actor is needed");
    Statement query = _db.Prepare(std::string("SELECT id, from_turn, to_turn, text, actor_id, conversation_id, told_by, "
                                              "source_id, superseded FROM summaries WHERE actor_id = ?1") +
                                  (includeSuperseded ? "" : " AND superseded = 0") + " ORDER BY from_turn, id");
    query.Bind(1, actorId);
    std::vector<core::Summary> summaries;
    while (query.Step()) {
        summaries.push_back(ReadSummaryRow(query));
    }
    return summaries;
}
```

with one shared reader (a private helper with several callers: `ActorSummaries`, `HostedSummaries`):

```cpp
// The summary in the current row of `query`, selected as id, from_turn, to_turn, text, actor_id, conversation_id,
// told_by, source_id, superseded. A hosted row without its actor is damaged saved data.
core::Summary RunStore::ReadSummaryRow(Statement& query) const
{
    core::Summary summary;
    summary.id = query.ColumnInt(0);
    summary.fromTurn = static_cast<int>(query.ColumnInt(1));
    summary.toTurn = static_cast<int>(query.ColumnInt(2));
    summary.text = query.ColumnText(3);
    summary.actorId = query.ColumnText(4);
    summary.conversationId = query.ColumnText(5);
    summary.toldBy = query.ColumnText(6);
    summary.sourceId = query.ColumnInt(7);
    summary.superseded = query.ColumnInt(8) != 0;
    if (summary.actorId.empty()) {
        throw StoreError(std::format("'{}': summary {}: actor_id is empty in a hosted run", core::PathText(_file),
                                     summary.id));
    }
    return summary;
}
```

(`HostedSummaries` selects every row, ordered by `from_turn, id`, and so throws on a row without its actor.)

The writers, each in one transaction where it changes two things, each checking `1 <= fromTurn <= toTurn` and a non-empty actor with `MORPHEUS_REQUIRE`, returning `_db`'s last inserted row id (use the `Database` method for it, or `SELECT last_insert_rowid()`):

- `AddActorSummary(summary)`: inserts every column but `id`.
- `RememberFold(summary)`: `AddActorSummary`, plus `SetRememberedThrough(summary.conversationId, summary.actorId, summary.toTurn)`, together; requires a non-empty `conversationId`.
- `FoldConversation(id, summary)`: as today (the folded mark only moves forward, never past the latest turn), now requiring `summary.actorId` non-empty and `summary.conversationId == id`; inserts through the same code as `RememberFold` and moves the conversation's `folded_through`, together. Returns the id.
- `CondenseActorSummaries(actorId, condensed)`: `UPDATE summaries SET superseded = 1 WHERE actor_id = ?1 AND superseded = 0`, then inserts `condensed` (its `actorId` must equal `actorId`, its `conversationId` and `toldBy` must be empty), together.
- `RememberedThrough(conversationId, actorId)`: the row's mark, 0 without one.
- `SetRememberedThrough(conversationId, actorId, turn)`: `INSERT ... ON CONFLICT(conversation_id, actor_id) DO UPDATE SET remembered_through = excluded.remembered_through`; `MORPHEUS_REQUIRE` the mark only moves forward and the conversation exists.
- `MarkFolded(conversationId, turn)`: moves `folded_through` forward (never back, never past the latest turn), without a summary: an ended conversation folded in the background.
- `AddPendingShare`, `PendingShares` (in id order), `RemovePendingShare`.

Make `AddSummary`, `ReplaceSummaries` and `Summaries` call `RequireOwned` and filter `WHERE actor_id = ''` (`ReplaceSummaries` deletes only those rows). `TruncateAfter` (owned only) is unchanged.

- [ ] **Step 6: Keep D1's hosted session compiling**

`turn::HostedSession` (src/turn/HostedSession.cpp) folds run-wide today; Task 4 gives it per-NPC memory. Until then, keep it working on the new store contract by treating the speaker as the fold's actor, the first step of per-speaker memory:

- `Upkeep` and `Pass` take the speaker's id. `Upkeep`'s fold sets `summary.actorId` to the speaker and `summary.conversationId` to the conversation, then calls `_store.FoldConversation`; `Pass` reads the speaker's shown summaries with `_store.ActorSummaries(speakerId, false)` instead of `_store.Summaries()`.
- `Maintain` folds an ended conversation as its last turn's speaker.
- `HostedSession::Summaries()` becomes `std::vector<core::Summary> Memories(std::string_view actorId)` (every summary of the actor, `ActorSummaries(actorId, true)`). Adapt the hosted tests that read `Summaries()` to read the speaker's memories; the fixtures' turns are spoken by Hulda unless a test says otherwise.

- [ ] **Step 7: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`, `"[hosted]"`, `"[turn]"`, `"[replay]"`.
Expected: `All tests passed` for each.

- [ ] **Step 8: Commit**

```bash
git add src/core/TurnRecord.h src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp tests/store/ tests/hosted/
git commit -m "Store version 5: summaries per actor, remembered-through marks and pending shares; version 4 hosted summaries move to their NPC witnesses"
```

---

