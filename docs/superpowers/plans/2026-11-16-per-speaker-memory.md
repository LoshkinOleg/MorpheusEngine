# Per-speaker memory (sub-project E1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In a hosted run every NPC remembers only what it witnessed and what other NPCs told it: per-NPC summaries in that NPC's words, the speaker's memories in the prompt's tail, retrieval limited to what the speaker witnessed, and an explicit share of one NPC's memories of a conversation with another.

**Architecture:** The store keeps summaries per actor (schema version 5) with a remembered-through mark per conversation and NPC, and pending shares. The shared memory upkeep (`turn::MemoryUpkeep`) reads and writes summaries only through a `SummaryScope`: Morpheus's own game passes today's run-wide scope (byte-identical behaviour), `turn::HostedSession` passes one NPC's. The hosted prompt moves memories from the system message to the tail, the retrieval index filters by witness, and `morpheus_server` gains two routes (API 1.1.0).

**Tech Stack:** C++20, CMake >= 3.25 (Visual Studio 2022, clang + Ninja), SQLite (`store`), nlohmann::json, cpp-httplib (server), Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`. Read it before Task 1. D1's and D2's specs (`docs/superpowers/specs/2026-11-16-hosted-world-design.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`) describe what this builds on.

## Global Constraints

- Morpheus's own game keeps its behaviour byte for byte: no `Session` test, replay fixture (`tests/data/replays`) or owned prompt changes. Run `"[turn]"`, `"[replay]"` and `"[context]"` after every task that touches `context` or `turn`.
- Only NPCs keep memories: an actor whose `is_player` is true in a turn's observation has none. A turn's witnesses are its speaker and its listeners.
- A summary belongs to one NPC (`actorId`) and one conversation (`conversationId`), except a condensed summary, which has no conversation and no teller (`toldBy`). A shared copy has `toldBy` set to the teller and `sourceId` set to the summary it copies.
- Condensing marks the NPC's shown summaries superseded and adds one condensed summary, in one transaction; superseded summaries stay in the store, are retrieved, and are found by shares.
- The hosted prompt, most stable first: L0 hosted system text; L1 lore and the participants; L3 the conversation's unfolded turns the speaker witnessed; the tail: `# What <name> remembers` with each shown summary oldest first (a shared one prefixed `(told by <teller's name>) `), then `# Relevant`, then the observation, the new events, the player's line and the instruction. No L2 in a hosted system message.
- `summary_cap` bounds the speaker's memories in the tail; a hosted run whose `summary_cap` is not below `tail_allowance` is a `core::DataError` from `HostedSession::Open` naming both keys. The owned game's settings are not checked for it.
- Retrieval for speaker X: the host's lore, the folded turns X witnessed, X's own summaries (shown or superseded, shared included). Nothing else.
- Store schema version 5. A version 4 hosted run's run-wide summaries are copied to every NPC who witnessed a turn in their range and removed; an owned run gains empty columns only.
- `api_version` becomes `"1.1.0"`. New routes: `POST /v1/runs/{run_id}/memories/share` (`{from, to, conversation_id}` -> `{shared, pending}`) and `GET /v1/runs/{run_id}/actors/{actor_id}/memories` (`{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}`).
- CLAUDE.md coding rules apply to every line: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for bad data, `store::StoreError` for damaged saved data naming the file and column); method bodies commented enough to read top to bottom; a private helper only with more than one caller (local lambdas are fine); class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS and grouped; types and methods PascalCase, locals camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; members initialized at declaration; RAII; no legacy paths kept; ASCII only in source comments and docs.
- Warnings are errors: MSVC `/W4 /permissive-`, Linux `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Cast between `std::size_t` / `std::int64_t` and `int` explicitly.
- Never range-for over a member of a temporary (`for (x : store.Conversation(id)->participants)` dangles in C++20): copy into a local first. Catch2 test names must not start with `/`. Catch2 macros only on the test thread.
- Tests are written first (Catch2). Fast loop: `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "<tag or name>"`; full: `ctest --preset tests`, and `ctest --preset asan` after `cmake --build --preset asan`.
- Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`. Commit messages end with the implementing model's trailer (`Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` or `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`).
- Before reporting, check every changed file with `LC_ALL=C grep -c $'[\x80-\xFF]' <file>` (must print 0) and leave no `morpheus_tests`, `morpheus_server` or `ctest` process running (`tasklist | grep -i -E "morpheus|ctest"`).

## File map

| File | Task | Responsibility |
|---|---|---|
| `src/core/TurnRecord.h` | 1 | `core::Summary` gains `actorId`, `conversationId`, `toldBy`, `sourceId`, `superseded`, `id` |
| `src/core/HostedRecord.h` | 1 | `core::PendingShare` |
| `src/store/RunStore.h/.cpp` | 1, 4 | Schema version 5, per-actor summary methods, marks, pending shares, `EmbeddingKind::Summary`, the version 4 upgrade |
| `src/turn/Folding.h/.cpp` | 2 | Fold and condense prompts take their instruction |
| `src/turn/MemoryUpkeep.h/.cpp` | 2 | `SummaryScope`; the upkeep reads and writes summaries only through it |
| `src/context/PromptBuilder.h/.cpp` | 2, 4 | `LayOut` knows where the summaries sit; the hosted prompt's memories in the tail |
| `src/turn/Session.cpp` | 2 | The run-wide scope (no behaviour change) |
| `src/turn/Retrieval.h/.cpp` | 3 | Witnessed turn entries, the summary source, the witness filter |
| `src/turn/EngineText.h` | 4 | `HOSTED_SUMMARIZE_INSTRUCTION`, `HOSTED_CONDENSE_INSTRUCTION`, `HOSTED_MEMORIES_HEADING` |
| `src/turn/HostedSession.h/.cpp` | 2, 3, 4, 5, 6 | Per-NPC memory: the speaker's fold, catch-up, `Maintain`, sharing, `Memories` |
| `src/server/Api.h/.cpp` | 7 | The two routes, `API_VERSION` 1.1.0 |
| `tests/hosted/fixtures/three_actors.json`, `tests/hosted/fixtures/README.md` | 5, 6 | R6's acceptance fixture and its format |
| `tests/hosted/HostedTestSupport.h` | 5 | The memory-echoing responder and the fixture player |
| Tests | each | `tests/store/HostedStoreTests.cpp`, `tests/turn/MemoryUpkeepTests.cpp`, `tests/context/HostedPromptTests.cpp`, `tests/turn/RetrievalTests.cpp` (or the file holding MemoryIndex tests), `tests/hosted/HostedMemoryTests.cpp`, `tests/hosted/HostedFixtureTests.cpp`, `tests/server/ApiMemoryTests.cpp`, `tests/server/HttpServerTests.cpp`, `tests/llama/LlamaHostedTests.cpp`, `tests/server/ServerLiveTests.cpp` |
| `README.md`, `CHANGELOG.md`, the specs | 8 | Documentation |

---

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

### Task 2: `SummaryScope`: the upkeep reads and writes summaries only through a scope

**Files:**
- Modify: `src/turn/Folding.h`, `src/turn/Folding.cpp` (instructions as parameters)
- Modify: `src/turn/MemoryUpkeep.h`, `src/turn/MemoryUpkeep.cpp`
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp` (`LayOut` knows where the summaries sit)
- Modify: `src/turn/Session.cpp` (the run-wide scope), `src/turn/HostedSession.cpp` (its scope)
- Test: `tests/turn/MemoryUpkeepTests.cpp`

**Interfaces:**
- Consumes: Task 1's store methods.
- Produces:
  - In `turn/MemoryUpkeep.h`:

```cpp
// Which summaries a fold and a condense work on (per-speaker memory spec, section 5): the owned game's run-wide
// summaries, or one NPC's in a hosted run. MemoryUpkeep reads and writes summaries only through it.
struct SummaryScope {
    // The scope's shown summaries, oldest first: what a condense replaces.
    std::function<std::vector<core::Summary>()> shown = {};
    // Stores a fold's summary (and makes its turns searchable history).
    std::function<void(const core::Summary& summary)> storeFold = {};
    // Replaces the shown summaries with `condensed`.
    std::function<void(const core::Summary& condensed)> storeCondensed = {};
    // The fold call's instruction.
    std::string foldInstruction = {};
    // The condense call's user message for `summaries` (the shown ones): the owned game's is the plain instruction (its
    // system message holds the summaries); a hosted one renders the memories before the instruction.
    std::function<std::string(const std::vector<core::Summary>& summaries)> condenseMessage = {};
};
```

  - `MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings, std::string tailName, int turn, int attempt, SummaryScope scope)` (replaces the `StoreFold` parameter; `StoreFold` goes).
  - `inference::Prompt SummarizePrompt(const inference::Message& systemMessage, const std::vector<context::TranscriptEntry>& entries, std::string_view instruction)`, `SummarizeTurns(systemMessage, entries, instruction, spec, backend, stop)`, `inference::Prompt CondensePrompt(const inference::Message& systemMessage, std::string_view message)`, `CondenseSummaries(systemMessage, summaries, message, spec, backend, stop)`.
  - `context::SummaryPlace { System, Tail }` in `context/PromptBuilder.h`; `LayOut` takes it (owned: `System`; hosted: `System` until Task 4 moves it).

- [ ] **Step 1: Write the failing test**

Add to `tests/turn/MemoryUpkeepTests.cpp` (read the file's helpers first; it builds a store, a scripted backend and an `Assemble` over transcript entries):

```cpp
TEST_CASE("MemoryUpkeep reads and writes summaries only through its scope, with the scope's instructions",
          "[turn][upkeep]")
{
    // A scope over a vector: folds and condenses land in it, never in the store, and the calls carry its texts.
    std::vector<core::Summary> kept = {{1, 2, "Earlier."}, {3, 4, "Later."}};
    turn::SummaryScope scope;
    scope.shown = [&kept] { return kept; };
    scope.storeFold = [&kept](const core::Summary& summary) { kept.push_back(summary); };
    scope.storeCondensed = [&kept](const core::Summary& condensed) { kept = {condensed}; };
    scope.foldInstruction = "Fold for the scope.";
    scope.condenseMessage = [](const std::vector<core::Summary>& summaries) {
        return std::format("Condense these {} for the scope.", summaries.size());
    };
    test::ScriptedBackend backend(16384, [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content.starts_with("Condense these") ? std::string("Condensed.")
                                                                           : std::string("Folded.");
    });
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_scope", 5, 8);

    // The prompt follows the vector: entries after its last summary are unfolded, and more than two summaries are over
    // the cap.
    turn::MemoryUpkeep::Assemble assemble = [&](const std::vector<turn::RetrievalHit>&) {
        const int foldedThrough = kept.empty() ? 0 : kept.back().toTurn;
        turn::FitPass pass;
        for (const context::TranscriptEntry& entry : harness.entries) {
            if (entry.turn > foldedThrough) {
                pass.transcript.push_back(entry);
            }
        }
        pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
        pass.assembly.prompt.stableCount = 1;
        pass.assembly.summaryOverBudget = kept.size() > 2;
        return pass;
    };
    turn::MemoryUpkeep upkeep(harness.store, backend, settings, "the test tail", 9, 0, scope);
    CHECK(upkeep.FoldAll(assemble, {}));

    // 5-8 fold in one call with the scope's instruction; three summaries over the cap condense into one with the
    // scope's message; the store's run-wide summaries are untouched.
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].fromTurn == 1);
    CHECK(kept[0].toTurn == 8);
    CHECK(kept[0].text == "Condensed.");
    REQUIRE(backend.Calls().size() == 2);
    CHECK(backend.Calls()[0].prompt.messages.back().content == "Fold for the scope.");
    CHECK(backend.Calls()[1].prompt.messages.back().content == "Condense these 3 for the scope.");
    CHECK(harness.store.Summaries().empty());
}
```

Update `Harness::Upkeep` to pass a run-wide scope like `Session`'s (Step 4), keeping `folded` recorded:

```cpp
    turn::MemoryUpkeep Upkeep(test::ScriptedBackend& backend, const core::Settings& settings)
    {
        turn::SummaryScope scope;
        scope.shown = [this] { return store.Summaries(); };
        scope.storeFold = [this](const core::Summary& summary) {
            store.AddSummary(summary);
            folded.push_back(summary);
        };
        scope.storeCondensed = [this](const core::Summary& condensed) { store.ReplaceSummaries(condensed); };
        scope.foldInstruction = std::string(turn::SUMMARIZE_INSTRUCTION);
        scope.condenseMessage = [](const std::vector<core::Summary>&) { return std::string(turn::CONDENSE_INSTRUCTION); };
        return turn::MemoryUpkeep(store, backend, settings, "the test tail", 9, 0, std::move(scope));
    }
```

Every other test in the file stays as it is.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`'SummaryScope': is not a member of 'morpheus::turn'`).

- [ ] **Step 3: Folding takes its instructions**

In `src/turn/Folding.h/.cpp`: `SummarizePrompt`, `SummarizeTurns`, `CondensePrompt` and `CondenseSummaries` take the instruction (or the condense message) as a `std::string_view` parameter placed as in **Produces**, and use it where they used `SUMMARIZE_INSTRUCTION` / `CONDENSE_INSTRUCTION`. Update their comments ("then `instruction`"). Update every caller (`grep -rn "SummarizePrompt\|SummarizeTurns\|CondensePrompt\|CondenseSummaries" src tests`).

- [ ] **Step 4: The upkeep works through the scope**

In `MemoryUpkeep`: the member `StoreFold _storeFold` becomes `SummaryScope _scope`; the constructor requires every function of the scope and both texts (`MORPHEUS_REQUIRE`, "MemoryUpkeep: a scope needs its summaries, its stores and its instructions"). `FoldOldest` calls `SummarizePrompt(systemMessage, oldest(count), _scope.foldInstruction)` and `SummarizeTurns(..., _scope.foldInstruction, ...)`, then `_scope.storeFold(*summary)`. `Condense` reads `const std::vector<core::Summary> summaries = _scope.shown();`, builds `const std::string message = _scope.condenseMessage(summaries);`, measures `CondensePrompt(systemMessage, message)`, calls `CondenseSummaries(systemMessage, summaries, message, ...)`, and stores with `_scope.storeCondensed(*merged)` instead of `_pStore->ReplaceSummaries`. `_pStore` stays for the run's seed (`Meta().rngSeed`).

In `src/turn/Session.cpp`, `Session::Upkeep` passes the run-wide scope:

```cpp
    // The run-wide scope: the owned run's summaries and the engine's texts, as before the scope existed.
    SummaryScope scope;
    scope.shown = [this] { return _store.Summaries(); };
    scope.storeFold = [this](const core::Summary& summary) {
        _store.AddSummary(summary);
        _index.SyncHistory(_store, summary.toTurn, *_pBackend);
    };
    scope.storeCondensed = [this](const core::Summary& condensed) { _store.ReplaceSummaries(condensed); };
    scope.foldInstruction = std::string(SUMMARIZE_INSTRUCTION);
    scope.condenseMessage = [](const std::vector<core::Summary>&) { return std::string(CONDENSE_INSTRUCTION); };
    return MemoryUpkeep(_store, *_pBackend, _settings, "the input and the state", turn, attempt, std::move(scope));
```

In `src/turn/HostedSession.cpp`, `Upkeep` passes the speaker's scope with the same texts (Task 4 gives hosted runs their own texts): `shown` reads `_store.ActorSummaries(speakerId, false)`, `storeFold` is Task 1's fold (with the actor and conversation set, then `SyncHistory()`), `storeCondensed` sets `actorId` and calls `_store.CondenseActorSummaries(speakerId, condensed)`.

- [ ] **Step 5: `LayOut` knows where the summaries sit**

In `src/context/PromptBuilder.h`:

```cpp
// Where a prompt's summaries sit: in the system message (the owned game's L2) or in the tail (a hosted speaker's
// memories). LayOut counts an over-cap summary at most at its cap where it sits, so condensing, not a settings error,
// answers it.
enum class SummaryPlace { System, Tail };
```

`LayOut` takes `SummaryPlace place` after `budget`. With `System`, it computes exactly what it computes today. With `Tail`:

```cpp
    // A hosted speaker's memories sit in the tail: an over-cap summary is counted at most at its cap there, and the
    // system message holds none.
    const int summaryExcess = std::max(0, tokens.summary - budget.summaryCap);
    assembly.tailOverBudget = (place == SummaryPlace::Tail ? tokens.tail - summaryExcess : tokens.tail) >
                              budget.tailAllowance;
    const int systemShare = place == SummaryPlace::System ? systemCost - summaryExcess : systemCost;
```

and use `systemShare` where the transcript budget and its error used `systemCost - summaryExcess`. `AssembleResolvePrompt` passes `SummaryPlace::System`; `AssembleHostedPrompt` passes `SummaryPlace::System` for now (Task 4 moves its summaries).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[upkeep]"`, `"[turn]"`, `"[replay]"`, `"[context]"`, `"[hosted]"`.
Expected: `All tests passed` for each, with no existing `Session` test or replay edited.

- [ ] **Step 7: Commit**

```bash
git add src/turn/Folding.h src/turn/Folding.cpp src/turn/MemoryUpkeep.h src/turn/MemoryUpkeep.cpp src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/Session.cpp src/turn/HostedSession.cpp tests/turn/MemoryUpkeepTests.cpp
git commit -m "SummaryScope: the memory upkeep reads and writes summaries through a scope; the owned game passes the run-wide one"
```

---

### Task 3: Retrieval by witness, and summaries as a searchable source

**Files:**
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`
- Modify: `src/turn/HostedSession.cpp` (`SyncHistory` passes witnesses and indexes summaries; `RunTurn` retrieves for the speaker)
- Test: `tests/turn/RetrievalTests.cpp`

**Interfaces:**
- Consumes: `store::EmbeddingKind::Summary`, `RunStore::HostedSummaries()` (Task 1).
- Produces:
  - `struct turn::WitnessedEntry { context::TranscriptEntry entry = {}; std::vector<std::string> witnesses = {}; };`
  - `void MemoryIndex::SyncEntries(const std::vector<WitnessedEntry>& folded, store::RunStore& store, inference::LlmBackend& backend)` (replaces the `TranscriptEntry` overload).
  - `void MemoryIndex::SyncSummaries(const std::vector<core::Summary>& summaries, store::RunStore& store, inference::LlmBackend& backend)`: replaces every summary entry; each embedded as its text cut to `EMBED_TEXT_CHARS`, shown as `Memory of turns A-B: <text>` (text cut to `2 * HISTORY_SNIPPET_CHARS`), refId the summary's id in decimal, owned by `summary.actorId`.
  - `MemoryIndex::Retrieve(query, sources, capTokens, maxHits, backend, std::optional<std::string_view> witness = std::nullopt)`: with a witness, a turn entry is searched only when the witness is among its witnesses, and a summary entry only when the witness owns it. Lore and notes are unaffected. Without one, behaviour is unchanged.

- [ ] **Step 1: Write the failing test**

Add to `tests/turn/RetrievalTests.cpp` (add the includes it needs: `<array>`, `<set>`, `<utility>`, `"context/Transcript.h"`, `"core/TurnRecord.h"`):

```cpp
TEST_CASE("Retrieve for a witness returns only the turns it witnessed and the summaries it owns", "[retrieval][memory]")
{
    test::ScriptedBackend backend(16384);
    store::RunStore store = store::RunStore::Create(test::FreshTempDir("retrieval_witness") / "run.db",
                                                    store::RunMeta{}, nlohmann::json::object());
    turn::MemoryIndex index("search_document: ");
    auto turnEntry = [](int number, const std::string& fact) {
        context::TranscriptEntry entry;
        entry.turn = number;
        entry.fact = fact;
        return entry;
    };
    index.SyncEntries({{turnEntry(1, "bridge one"), {"hulda", "saadia"}}, {turnEntry(2, "bridge two"), {"hulda"}}},
                      store, backend);
    core::Summary mine{1, 1, "bridge three"};
    mine.actorId = "saadia";
    mine.id = 7;
    core::Summary theirs{2, 2, "bridge four"};
    theirs.actorId = "hulda";
    theirs.id = 8;
    index.SyncSummaries({mine, theirs}, store, backend);

    const std::array<store::EmbeddingKind, 2> sources = {store::EmbeddingKind::Turn, store::EmbeddingKind::Summary};
    turn::RetrievalQuery query;
    query.input = "bridge";
    // The hits as (kind, refId) pairs.
    auto found = [](const std::vector<turn::RetrievalHit>& hits) {
        std::set<std::pair<store::EmbeddingKind, std::string>> pairs;
        for (const turn::RetrievalHit& hit : hits) {
            pairs.insert({hit.kind, hit.refId});
        }
        return pairs;
    };
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend)).size() == 4);
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend, "saadia")) ==
          std::set<std::pair<store::EmbeddingKind, std::string>>{{store::EmbeddingKind::Turn, "1"},
                                                                 {store::EmbeddingKind::Summary, "7"}});
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend, "belethor")).empty());

    // A summary is shown under its range.
    const std::vector<turn::RetrievalHit> hulda = index.Retrieve(query, sources, 1000, 10, backend, "hulda");
    const bool shown = std::any_of(hulda.begin(), hulda.end(), [](const turn::RetrievalHit& hit) {
        return hit.kind == store::EmbeddingKind::Summary && hit.text == "Memory of turns 2-2: bridge four";
    });
    CHECK(shown);
}
```

Update the file's existing calls of `SyncEntries` to the `WitnessedEntry` form (witnesses empty: they are searched only without a witness filter).

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`SyncSummaries` is not a member of `MemoryIndex`).

- [ ] **Step 3: The index**

In `src/turn/Retrieval.h`, before `class MemoryIndex`:

```cpp
// A hosted run's folded turn as searchable history, with who witnessed it (its speaker and listeners).
struct WitnessedEntry {
    context::TranscriptEntry entry = {};
    std::vector<std::string> witnesses = {};
};
```

`Entry` gains, last: `std::vector<std::string> witnesses = {}; // Turn: its speaker and listeners. Summary: its owner. Empty otherwise.` Declare `SyncEntries(const std::vector<WitnessedEntry>&, ...)`, `SyncSummaries(...)` and the `witness` parameter of `Retrieve`, with comments as in **Produces**.

In `src/turn/Retrieval.cpp`:

```cpp
void MemoryIndex::SyncEntries(const std::vector<WitnessedEntry>& folded, store::RunStore& store,
                              inference::LlmBackend& backend)
{
    // Each folded turn under its run-wide number, with its witnesses: its fact is what is embedded and, cut shorter,
    // what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const WitnessedEntry& witnessed : folded) {
        const context::TranscriptEntry& turn = witnessed.entry;
        const std::string refId = std::to_string(turn.turn);
        documents.push_back({refId, core::CutUtf8(turn.fact, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Turn, refId, {},
                           std::format("Turn {}: {}", turn.turn, core::CutUtf8(turn.fact, 2 * HISTORY_SNIPPET_CHARS)),
                           {}, witnessed.witnesses});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Turn, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Turn, std::move(entries));
}

void MemoryIndex::SyncSummaries(const std::vector<core::Summary>& summaries, store::RunStore& store,
                                inference::LlmBackend& backend)
{
    // Each NPC summary under its id, owned by its NPC: its text is what is embedded and, cut shorter, what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const core::Summary& summary : summaries) {
        MORPHEUS_REQUIRE(!summary.actorId.empty() && summary.id > 0,
                         "MemoryIndex::SyncSummaries: a stored NPC summary is needed");
        const std::string refId = std::to_string(summary.id);
        documents.push_back({refId, core::CutUtf8(summary.text, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Summary, refId, {},
                           std::format("Memory of turns {}-{}: {}", summary.fromTurn, summary.toTurn,
                                       core::CutUtf8(summary.text, 2 * HISTORY_SNIPPET_CHARS)),
                           {}, {summary.actorId}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Summary, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Summary, std::move(entries));
}
```

In `Retrieve`, the `searched` lambda also applies the witness, and the `anything` check uses it, so a witness with nothing it may see makes no embedding call:

```cpp
    // An entry is searched when its kind is one of the sources and, for a witness, when it may see it: a turn it
    // witnessed, or a summary it owns. Lore and notes are everyone's.
    auto searched = [&sources, &witness](const Entry& entry) {
        if (std::find(sources.begin(), sources.end(), entry.kind) == sources.end()) {
            return false;
        }
        if (!witness || (entry.kind != store::EmbeddingKind::Turn && entry.kind != store::EmbeddingKind::Summary)) {
            return true;
        }
        return std::find(entry.witnesses.begin(), entry.witnesses.end(), *witness) != entry.witnesses.end();
    };
```

(`entry.witnesses` holds `std::string`; compare with `std::string_view` by `std::find_if(... [&](const std::string& id) { return id == *witness; })` if `std::find` does not compile.) Update the two uses of `searched` to pass the entry.

- [ ] **Step 4: The hosted session indexes witnesses and summaries, and retrieves for the speaker**

In `src/turn/HostedSession.cpp`:

- `HOSTED_SOURCES` becomes `{Lore, Turn, Summary}` (size 3).
- `SyncHistory` builds `WitnessedEntry`s: for each conversation, its folded entries (`Entries(conversation, true)`) with each turn's witnesses (`turn.speakerId` plus `turn.listenerIds`, from `_store.HostedTurns(conversation.id)` by turn number), then calls `_index.SyncEntries(folded, ...)` and `_index.SyncSummaries(_store.HostedSummaries(), _store, *_pBackend)`.
- `RunTurn`'s `_index.Retrieve(...)` passes `request.speakerId` as the witness.
- The `storeCondensed` of the scope (Task 2) calls `SyncHistory()` after `CondenseActorSummaries`, so the condensed summary is searchable.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[retrieval]"`, `"[hosted]"`, `"[turn]"`, `"[replay]"`.
Expected: `All tests passed` for each.

- [ ] **Step 6: Commit**

```bash
git add src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/HostedSession.cpp tests/turn/RetrievalTests.cpp
git commit -m "Retrieval by witness: folded turns carry their witnesses, NPC summaries are searchable by their owner"
```

---

### Task 4: The speaker's memories in the tail, the speaker's L3, and the speaker's fold

**Files:**
- Modify: `src/context/PromptBuilder.h`, `src/context/PromptBuilder.cpp` (hosted inputs and rendering)
- Modify: `src/turn/EngineText.h` (two hosted texts)
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Test: `tests/context/HostedPromptTests.cpp`, `tests/hosted/HostedMemoryTests.cpp` (new), and the hosted tests whose expectations move with L2

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  - `struct context::MemoryLine { std::string toldBy = {}; std::string text = {}; };` (`toldBy` is the teller's name, empty for the speaker's own).
  - `context::HostedInputs` loses `summaries` and gains `std::string speakerName = {};` and `std::vector<MemoryLine> memories = {};`.
  - `std::string context::RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories)`: `"# What <name> remembers\n"` then `"- (told by <teller>) <text>\n"` or `"- <text>\n"` per memory; `""` for none.
  - `turn::HOSTED_SUMMARIZE_INSTRUCTION`, `turn::HOSTED_CONDENSE_INSTRUCTION` (each with `{actor}`).
  - `HostedSession` private: `std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, int afterTurn, int throughTurn, std::string_view witnessId)` (turns with `afterTurn < number <= throughTurn`, only those `witnessId` witnessed when it is not empty), `FitPass Pass(std::string_view conversationId, std::string_view actorId, std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits, context::HostedInputs inputs)`, `MemoryUpkeep Upkeep(const std::string& conversationId, const std::string& actorId, int turn, bool foldsConversation)`.
  - `HostedSession::Open` throws `core::DataError` naming `summary_cap` and `tail_allowance` when `summary_cap >= tail_allowance`.

- [ ] **Step 1: Write the failing tests**

In `tests/context/HostedPromptTests.cpp` (read its helpers first: it builds `HostedInputs` and calls `AssembleHostedPrompt` with a fake backend), add:

```cpp
TEST_CASE("A hosted prompt shows the speaker's memories in the tail, with their tellers, and no L2", "[context][hosted]")
{
    context::HostedInputs inputs = ExampleHostedInputs(); // The file's helper; use its name.
    inputs.speakerName = "Saadia";
    inputs.memories = {{"", "Saadia arrived in Whiterun in spring."}, {"Hulda", "The guard captain takes bribes."}};
    test::FakeBackend backend(16384);
    const context::Assembly assembly = context::AssembleHostedPrompt(inputs, ExampleBudget(), backend);
    const std::string& system = assembly.prompt.messages.front().content;
    const std::string& tail = assembly.prompt.messages.back().content;
    CHECK(system.find("arrived in Whiterun") == std::string::npos);
    CHECK(tail.starts_with("# What Saadia remembers\n- Saadia arrived in Whiterun in spring.\n- (told by Hulda) The guard "
                           "captain takes bribes.\n"));
    CHECK(assembly.tokens.summary ==
          backend.CountTokens(context::RenderMemories("Saadia", inputs.memories)));
    // No memories: no heading.
    inputs.memories.clear();
    const context::Assembly empty = context::AssembleHostedPrompt(inputs, ExampleBudget(), backend);
    CHECK(empty.prompt.messages.back().content.find("remembers") == std::string::npos);
}
```

(Use the file's helpers for a valid `HostedInputs` and budget; adapt the names `ExampleHostedInputs` and `ExampleBudget` to the ones it has. Update its existing tests that expected the summaries in the system message: they now expect them, as memories, in the tail.)

Create `tests/hosted/HostedMemoryTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

} // namespace

TEST_CASE("Two NPCs taking turns in one conversation share the prompt's prefix up to the tail", "[hosted][memory]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"Welcome.", "Hello there.", "Good day."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_prefix"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "And you?"), {}, {}));
    const inference::Prompt hulda = session.LastAssembly().prompt;
    REQUIRE(session.RunTurn(test::Request(id, test::SAADIA, "And you, Saadia?"), {}, {}));
    const inference::Prompt saadia = session.LastAssembly().prompt;

    // Saadia's prompt starts with everything Hulda's stable prefix held.
    REQUIRE(saadia.messages.size() >= hulda.stableCount);
    for (std::size_t k = 0; k < hulda.stableCount; ++k) {
        INFO("message " << k);
        CHECK(saadia.messages[k].role == hulda.messages[k].role);
        CHECK(saadia.messages[k].content == hulda.messages[k].content);
    }
}

TEST_CASE("A fold during a turn writes the speaker's memory, in the speaker's words", "[hosted][memory]")
{
    // A transcript budget small enough that the third turn folds the oldest turns.
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {std::string(3000, 'a'), std::string(3000, 'b'), "Short."};
    test::ScriptedBackend backend(4096, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = 4096;
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_memory_fold"),
                                                            test::FixtureHostData(), settings, 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "One."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Two."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Three."), {}, {}));

    // Hulda, the speaker, remembers the folded turns; the fold call named her.
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const std::vector<core::Summary> hulda = session.Memories(test::HULDA);
    REQUIRE_FALSE(hulda.empty());
    CHECK(hulda[0].conversationId == id);
    bool named = false;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        named = named || call.prompt.messages.back().content.find("as Hulda remembers") != std::string::npos;
    }
    CHECK(named);
}

TEST_CASE("A hosted run's summary_cap must be below its tail_allowance", "[hosted][memory]")
{
    test::HostedScript script;
    test::ScriptedBackend backend(16384, script.Responder());
    core::Settings settings = test::HostedSettings();
    settings.budget.summaryCap = settings.budget.tailAllowance;
    CHECK_THROWS_AS(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap"), test::FixtureHostData(), settings,
                                              42, backend),
                    core::DataError);
    CHECK_THROWS_WITH(turn::HostedSession::Open(test::HostedRunFile("hosted_memory_cap2"), test::FixtureHostData(),
                                                settings, 42, backend),
                      ContainsSubstring("summary_cap") && ContainsSubstring("tail_allowance"));
}
```

(The fold test's sizes: with a 4096 context and HostedSettings' budgets, two 3000-byte lines (750 tokens each) put L3 over its budget on the third turn. If the fake counts differently, tune the line lengths so `foldedThrough` moves, and say so in the report. If HostedSettings' budgets do not leave room for L3 in 4096 tokens, use 8192 and 6000-byte lines.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`speakerName` is not a member of `HostedInputs`).

- [ ] **Step 3: The hosted prompt**

In `src/context/PromptBuilder.h`, before `HostedInputs`:

```cpp
// One memory of a hosted speaker, as its prompt shows it: its text, and the teller's name for one told by another NPC.
struct MemoryLine {
    std::string toldBy = {}; // Empty for the speaker's own memory.
    std::string text = {};
};
```

In `HostedInputs`, remove `summaries` and add, before `retrieved`:

```cpp
    std::string speakerName = {};          // The speaker, for the memories' heading.
    std::vector<MemoryLine> memories = {}; // Tail: the speaker's shown summaries, oldest first.
```

Declare after `AssembleHostedPrompt`:

```cpp
// The speaker's memories as a hosted tail shows them: "# What <name> remembers", then one line per memory ("- (told by
// <teller>) <text>" for one told by another NPC). Empty for none. The condense call of a hosted NPC shows the same.
std::string RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories);
```

In `src/context/PromptBuilder.cpp`:

```cpp
std::string RenderMemories(std::string_view speakerName, const std::vector<MemoryLine>& memories)
{
    if (memories.empty()) {
        return {};
    }
    std::string text = std::format("# What {} remembers\n", speakerName);
    for (const MemoryLine& memory : memories) {
        text += memory.toldBy.empty() ? std::format("- {}\n", memory.text)
                                      : std::format("- (told by {}) {}\n", memory.toldBy, memory.text);
    }
    return text;
}
```

In `AssembleHostedPrompt`: drop the L2 summary text from the system message (the system text is `inputs.systemText + "\n\n" + worldText`), and start the tail with the memories:

```cpp
    // The tail, rebuilt every turn, so it goes last: the speaker's memories (they change with the speaker, so they sit
    // after the conversation the speakers share), retrieved snippets, the observation, the new events, the player's
    // line and the instruction.
    const std::string memories = RenderMemories(inputs.speakerName, inputs.memories);
    std::string tail = memories.empty() ? std::string() : memories + "\n";
```

and set `tokens.summary = backend.CountTokens(memories);` where it counted the summary text, then call `LayOut(..., budget, SummaryPlace::Tail, backend)`.

- [ ] **Step 4: The hosted texts**

In `src/turn/EngineText.h`, with the other hosted texts:

```cpp
// A hosted NPC's fold call (per-speaker memory spec, section 1): its memory of the turns above, in its own terms.
// {actor} is the NPC's name.
inline constexpr std::string_view HOSTED_SUMMARIZE_INSTRUCTION =
    "Summarize the turns above as {actor} remembers them, for {actor}'s own memory, not for the player. Keep names, "
    "places, items, promises, secrets and who said what to whom. Leave out JSON and engine details. Plain prose, at "
    "most 200 words.";

// A hosted NPC's condense call: the memories above (RenderMemories) become one. {actor} is the NPC's name.
inline constexpr std::string_view HOSTED_CONDENSE_INSTRUCTION =
    "Condense what {actor} remembers, above, into one summary for {actor}'s own memory, not for the player. Keep names, "
    "places, items, promises, secrets, who told {actor} what, and the order of events. Plain prose, at most 250 words.";
```

- [ ] **Step 5: The speaker's L3, memories and fold**

In `src/turn/HostedSession.h`, replace the private `Entries`, `Pass` and `Upkeep` declarations with those in **Produces**, with comments:

```cpp
    // A conversation's turns with afterTurn < number <= throughTurn as transcript entries, oldest first, only those
    // `witnessId` witnessed (its speaker or a listener) when it is not empty. Rendering as before: a turn's user
    // message is the events before it (recorded since the conversation's previous turn, then its request's own, less
    // the ones its prompt left out) in parentheses and the player's line; HOSTED_TURN_CUE when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>". Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, int afterTurn,
                                                  int throughTurn, std::string_view witnessId);

    // One pass of the memory upkeep for `actorId` in conversation `conversationId`: the hosted prompt with
    // `transcript` as L3, `actorId`'s shown summaries as its memories (tellers by name), `hits` and the tail parts
    // `inputs` already holds, read from the store as it is now.
    FitPass Pass(std::string_view conversationId, std::string_view actorId, std::vector<context::TranscriptEntry> transcript,
                 const std::vector<RetrievalHit>& hits, context::HostedInputs inputs);

    // The shared memory upkeep for `actorId`'s memory of conversation `conversationId`, its seeds naming turn `turn`:
    // `actorId`'s scope with the hosted texts. A fold's summary is the actor's; with foldsConversation it also moves
    // the conversation's folded mark (a fold during the actor's turn), otherwise only the actor's own (a catch-up).
    MemoryUpkeep Upkeep(const std::string& conversationId, const std::string& actorId, int turn, bool foldsConversation);
```

In `src/turn/HostedSession.cpp`:

- `Open`: before anything else touches the file, after `HostedWorld world(data);`:

```cpp
    // The speaker's memories sit in the tail, capped by summary_cap, so the tail must have room for more than them.
    if (settings.budget.summaryCap >= settings.budget.tailAllowance) {
        throw core::DataError(std::format("settings: summary_cap {} must be below tail_allowance {} for a hosted run: "
                                          "the speaker's memories sit in the tail",
                                          settings.budget.summaryCap, settings.budget.tailAllowance));
    }
```

- `Entries(conversation, afterTurn, throughTurn, witnessId)`: the current body, with the condition `(turn.number <= conversation.foldedThrough) == folded` replaced by `turn.number > afterTurn && turn.number <= throughTurn && witnessed`, where `witnessed` is true when `witnessId` is empty, is the turn's speaker, or is among `turn.listenerIds`. `previous` still follows every turn of the conversation (events are placed by the conversation's turns, whoever witnessed them).
- `Pass(conversationId, actorId, transcript, hits, inputs)`:

```cpp
FitPass HostedSession::Pass(std::string_view conversationId, std::string_view actorId,
                            std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its marks.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    // An actor's name: as a participant of this conversation, else in the observation, else its id.
    const std::vector<core::Actor> observed = HostedWorld::ActorsOf(inputs.observation);
    auto nameOf = [&conversation, &observed](std::string_view id) {
        for (const core::Participant& participant : conversation->participants) {
            if (participant.id == id) {
                return participant.name;
            }
        }
        for (const core::Actor& actor : observed) {
            if (actor.id == id) {
                return actor.name;
            }
        }
        return std::string(id);
    };
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    inputs.speakerName = nameOf(actorId);
    inputs.memories.clear();
    for (const core::Summary& summary : _store.ActorSummaries(actorId, false)) {
        inputs.memories.push_back({summary.toldBy.empty() ? std::string() : nameOf(summary.toldBy), summary.text});
    }
    inputs.transcript = std::move(transcript);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}
```

  (`HostedWorld::ActorsOf` on the maintenance placeholder's observation works: it is the last turn's observation. If `inputs.observation` can be null at a call site, guard `ActorsOf` with `inputs.observation.is_object()`.)

- `Upkeep(conversationId, actorId, turn, foldsConversation)`:

```cpp
MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, const std::string& actorId, int turn,
                                   bool foldsConversation)
{
    // The actor's name in its texts: as a participant of the conversation, else its id.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Upkeep: the conversation is not in the store");
    std::string name = actorId;
    for (const core::Participant& participant : conversation->participants) {
        if (participant.id == actorId) {
            name = participant.name;
        }
    }
    // {actor} in a hosted text is the actor's name.
    auto fill = [&name](std::string_view text) {
        std::string filled(text);
        for (std::size_t at = filled.find(ACTOR_VARIABLE); at != std::string::npos; at = filled.find(ACTOR_VARIABLE)) {
            filled.replace(at, ACTOR_VARIABLE.size(), name);
        }
        return filled;
    };

    SummaryScope scope;
    scope.shown = [this, actorId] { return _store.ActorSummaries(actorId, false); };
    scope.storeFold = [this, conversationId, actorId, foldsConversation](core::Summary summary) {
        // The actor's memory of the conversation; a fold during its turn also cuts the shared transcript there.
        summary.actorId = actorId;
        summary.conversationId = conversationId;
        if (foldsConversation) {
            _store.FoldConversation(conversationId, summary);
        } else {
            _store.RememberFold(summary);
        }
        SyncHistory();
    };
    scope.storeCondensed = [this, actorId](core::Summary condensed) {
        condensed.actorId = actorId;
        _store.CondenseActorSummaries(actorId, condensed);
        SyncHistory();
    };
    scope.foldInstruction = fill(HOSTED_SUMMARIZE_INSTRUCTION);
    scope.condenseMessage = [this, name, actorId, fill](const std::vector<core::Summary>& summaries) {
        // The memories as the actor's prompt shows them (tellers by id: a condense only needs who told what), then
        // the instruction.
        std::vector<context::MemoryLine> lines;
        for (const core::Summary& summary : summaries) {
            lines.push_back({summary.toldBy, summary.text});
        }
        return context::RenderMemories(name, lines) + "\n" + fill(HOSTED_CONDENSE_INSTRUCTION);
    };
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0, std::move(scope));
}
```

  with `constexpr std::string_view ACTOR_VARIABLE = "{actor}";` beside the other variables. (`storeFold`'s parameter is `const core::Summary&` in `SummaryScope`; take it by const reference and copy into a local before setting the fields.)

- `RunTurn`: the fit becomes

```cpp
    // The speaker's L3 is the conversation's unfolded turns it witnessed, read again each pass (a fold moves the
    // conversation's mark); a fold during this turn is the speaker's memory and cuts the shared transcript.
    auto speakerPass = [&](const std::vector<RetrievalHit>& kept) {
        const core::Conversation now = *_store.Conversation(conversation.id);
        return Pass(conversation.id, request.speakerId,
                    Entries(now, now.foldedThrough, _store.LatestHostedTurn(), request.speakerId), kept, tail);
    };
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, request.speakerId, turn, true).Fit(
        hits,
        [&](const std::vector<RetrievalHit>& kept) {
            FitPass pass = speakerPass(kept);
            while (pass.assembly.tailOverBudget && kept.empty() && !tail.events.empty()) {
                tail.events.erase(tail.events.begin());
                pass = speakerPass(kept);
            }
            return pass;
        },
        stop);
```

- `SyncHistory` uses `Entries(conversation, 0, conversation.foldedThrough, "")` for the folded turns.
- `Maintain` (until Task 5 replaces it): an ended conversation's remaining turns fold as its last turn's speaker's memory, with `Upkeep(conversation.id, lastSpeaker, latest, true)` and an `assemble` returning `Pass(conversation.id, lastSpeaker, Entries(now, now.foldedThrough, latest, lastSpeaker), {}, tail)`.

In `tests/hosted/HostedTestSupport.h`, `HostedScript::Responder` answers "Summary." for the hosted memory calls too, or every hosted fold would be taken for a speak call:

```cpp
            const std::string& last = prompt.messages.back().content;
            // Memory calls: the owned game's texts, and the hosted ones (which name the NPC).
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION ||
                last.find("Summarize the turns above as") != std::string::npos ||
                last.find("Condense what") != std::string::npos) {
                return "Summary.";
            }
```

Update the hosted tests whose expectations depended on L2 in the system message (`grep -rn "Summaries()\|summaries" tests/hosted tests/context`): they now find the speaker's memories in the tail. Update the D1 fixtures' `l3` expectations only if a fixture turn is spoken by an NPC who did not witness an earlier turn (none should be: check, and report).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`, `"[context]"`, `"[turn]"`, `"[replay]"`, `"[hosted-server]"`.
Expected: `All tests passed` for each.

- [ ] **Step 7: Commit**

```bash
git add src/context/PromptBuilder.h src/context/PromptBuilder.cpp src/turn/EngineText.h src/turn/HostedSession.h src/turn/HostedSession.cpp tests/context/ tests/hosted/
git commit -m "Hosted prompts: the speaker's memories in the tail, the speaker's witnessed L3, and a fold during a turn is the speaker's memory"
```

---

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

### Task 7: The server: share and memories routes, API 1.1.0, the three-actor fixture over HTTP

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (two routes)
- Modify: `CHANGELOG.md`
- Test: `tests/server/ApiMemoryTests.cpp` (new), `tests/server/HttpMemoryTests.cpp` (new), and every server test that names `"1.0.0"`

**Interfaces:**
- Consumes: `HostedSession::ShareMemories`, `ShareResult`, `HostedSession::Memories` (Tasks 4-6); the server's `Api` helpers (`RequireConversation`, `RequiredString`, `NotFound`, `StatusOf`) and test helpers (`test::ApiHarness`, `test::HttpServer` use, `test::PostTurn`, `test::JsonPost`, `test::JsonGet`, `test::WaitUntil`) from D2.
- Produces: `POST /v1/runs/{run_id}/memories/share` with `{from, to, conversation_id}` -> `{shared, pending}`; `GET /v1/runs/{run_id}/actors/{actor_id}/memories` -> `{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}` oldest first; `server::API_VERSION == "1.1.0"`.

- [ ] **Step 1: Write the failing tests**

Change every `"1.0.0"` in `tests/server/` to `"1.1.0"` (`grep -rln '"1.0.0"' tests/server`).

Create `tests/server/ApiMemoryTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// Hulda's memories as GET answers them.
nlohmann::json HuldaMemories(test::ApiHarness& harness)
{
    return harness.Get("/v1/runs/save_1/actors/" + test::HULDA + "/memories").body.at("memories");
}

} // namespace

TEST_CASE("The memories of an actor read back, and a share copies them, told by the teller", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"The well is poisoned."};
    test::ApiHarness harness("api_memory_share", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    test::ReadAll(*harness
                       .Post("/v1/runs/save_1/conversations/c1/turn",
                             {{"speaker", test::HULDA}, {"player_text", "News?"}, {"observation", test::FixtureObservation()}})
                       .stream);

    // A share while the conversation is open waits.
    const server::ApiResponse early = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(early.status == 200);
    CHECK(early.body == nlohmann::json{{"shared", 0}, {"pending", true}, {"api_version", "1.1.0"}});

    // Ended, the conversation is folded in the background; the pending share then applies.
    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);
    REQUIRE(test::WaitUntil([&] {
        return harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories").size() == 1;
    }));
    const nlohmann::json hulda = HuldaMemories(harness);
    REQUIRE(hulda.size() == 1);
    CHECK(hulda[0].at("conversation_id") == "c1");
    CHECK(hulda[0].at("told_by") == "");
    CHECK(hulda[0].at("superseded") == false);
    CHECK_THAT(hulda[0].at("text").get<std::string>(), ContainsSubstring("poisoned"));
    const nlohmann::json saadia = harness.Get("/v1/runs/save_1/actors/" + test::SAADIA + "/memories").body.at("memories");
    CHECK(saadia[0].at("told_by") == test::HULDA);
    CHECK(saadia[0].at("from_turn") == 1);
    CHECK(saadia[0].at("to_turn") == 1);

    // Sharing again copies nothing new.
    const server::ApiResponse again = harness.Post("/v1/runs/save_1/memories/share",
                                                   {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}});
    CHECK(again.body.at("shared") == 0);
    CHECK(again.body.at("pending") == false);
}

TEST_CASE("Memory routes answer their errors with the API version", "[hosted-server][api][memory]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_memory_errors", test::MemoryEchoResponder(script));
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.1.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c9"}}),
          404, "c9");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::SAADIA}, {"to", test::HULDA}, {"conversation_id", "c1"}}),
          400, "from");
    check(harness.Post("/v1/runs/save_1/memories/share", {{"from", test::HULDA}, {"conversation_id", "c1"}}), 400, "to");
    check(harness.Post("/v1/runs/save_9/memories/share", {{"from", test::HULDA}, {"to", test::SAADIA}, {"conversation_id", "c1"}}),
          409, "save_9");
    check(harness.Get("/v1/runs/save_9/actors/" + test::HULDA + "/memories"), 409, "save_9");
    // An actor with no memories: an empty list.
    const server::ApiResponse none = harness.Get("/v1/runs/save_1/actors/0000BEEF/memories");
    CHECK(none.status == 200);
    CHECK(none.body.at("memories") == nlohmann::json::array());
}
```

(Saadia never witnessed `c1`'s turn, so she is refused as a teller with "from". The first share's 400 for `from` comes from `ShareMemories` before the turn exists: if `c1` has no turn yet, `NpcWitnesses` is empty and every teller is refused, which this test relies on; it runs no turn.)

Create `tests/server/HttpMemoryTests.cpp`: the three-actor fixture over real sockets, on scripted backends (the fixture's memory checks need the echo responder):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("The three-actor fixture plays over HTTP", "[hosted-server][http][memory][fixtures]")
{
    const nlohmann::json fixture = test::ReadFixture("three_actors.json");
    const nlohmann::json& observation = fixture.at("observation");
    test::HostedScript script;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    test::ApiHarness harness("http_memory_fixture", test::MemoryEchoResponder(script));
    server::HttpServer http(harness.api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    REQUIRE(test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json")).status == 200);
    auto start = [&](const nlohmann::json& participants) {
        const test::HttpJson started =
            test::JsonPost(client, "/v1/runs/r/conversations", {{"participants", participants}, {"observation", observation}});
        REQUIRE(started.status == 200);
        return started.body.at("conversation_id").get<std::string>();
    };
    std::string id = start(fixture.at("participants"));
    const std::string firstId = id;
    // An actor's memories as the server answers them.
    auto memoriesOf = [&](const std::string& actor) {
        return test::JsonGet(client, "/v1/runs/r/actors/" + actor + "/memories").body.at("memories");
    };

    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO("step " << i);
        const std::string base = "/v1/runs/r/conversations/" + id;
        if (step.contains("turn")) {
            nlohmann::json body = step.at("turn");
            body["observation"] = observation;
            const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body);
            REQUIRE(turn.status == 200);
            REQUIRE_FALSE(turn.events.empty());
            CHECK(turn.events.back().name == "done"); // The prompt checks need the session: HostedFixtureTests.
        } else if (step.contains("leave")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"left", step.at("leave")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("join")) {
            CHECK(test::JsonPost(client, base + "/participants", {{"joined", step.at("join")}, {"observation", observation}})
                      .status == 200);
        } else if (step.contains("end")) {
            CHECK(test::JsonPost(client, base + "/end", nlohmann::json::object()).status == 200);
        } else if (step.contains("maintain")) {
            // The server folds in the background: the memories checks below wait for what they expect.
        } else if (step.contains("memories")) {
            const nlohmann::json& check = step.at("memories");
            const std::string actor = check.at("actor").get<std::string>();
            // Every memory's text, once the expected ones have arrived.
            auto texts = [&] {
                std::string all;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    all += memory.at("text").get<std::string>() + "\n";
                }
                return all;
            };
            const nlohmann::json contains = check.value("contains", nlohmann::json::array());
            CHECK(test::WaitUntil([&] {
                const std::string all = texts();
                for (const nlohmann::json& present : contains) {
                    if (all.find(present.get<std::string>()) == std::string::npos) {
                        return false;
                    }
                }
                return true;
            }));
            const std::string all = texts();
            for (const nlohmann::json& absent : check.value("absent", nlohmann::json::array())) {
                CHECK_THAT(all, !ContainsSubstring(absent.get<std::string>()));
            }
            if (check.contains("count")) {
                CHECK(memoriesOf(actor).size() == check.at("count").get<std::size_t>());
            }
            for (const auto& item : check.value("told_by", nlohmann::json::object()).items()) {
                bool found = false;
                for (const nlohmann::json& memory : memoriesOf(actor)) {
                    if (memory.at("text").get<std::string>().find(item.key()) != std::string::npos) {
                        found = true;
                        CHECK(memory.at("told_by") == item.value());
                    }
                }
                CHECK(found);
            }
        } else if (step.contains("start")) {
            id = start(step.at("start"));
        } else if (step.contains("share")) {
            const nlohmann::json& share = step.at("share");
            const test::HttpJson shared = test::JsonPost(client, "/v1/runs/r/memories/share",
                                                         {{"from", share.at("from")},
                                                          {"to", share.at("to")},
                                                          {"conversation_id", share.value("conversation", firstId)}});
            REQUIRE(shared.status == 200);
            if (share.contains("shared")) {
                CHECK(shared.body.at("shared") == share.at("shared"));
            }
            if (share.contains("pending")) {
                CHECK(shared.body.at("pending") == share.at("pending"));
            }
        } else {
            FAIL("unknown step kind");
        }
    }
    harness.runs.StopAll();
    http.Stop();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: the new tests FAIL (404 "there is no route" for the two memory routes) and the renamed version checks FAIL ("1.0.0" answered).

- [ ] **Step 3: The routes**

In `src/server/Api.h`: `inline constexpr std::string_view API_VERSION = "1.1.0";`. Update the class comment's route list if it has one.

In `src/server/Api.cpp`, `Handle`, inside the `/v1/runs/{run_id}` branch, before the `conversations` branches:

```cpp
            } else if (post && parts.size() == 5 && parts[3] == "memories" && parts[4] == "share") {
                // One NPC tells another what it remembers of a conversation (per-speaker memory spec, section 2).
                const std::string from = RequiredString(json, "from");
                const std::string to = RequiredString(json, "to");
                const std::string conversationId = RequiredString(json, "conversation_id");
                turn::ShareResult result;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    RequireConversation(session, conversationId);
                    result = session.ShareMemories(from, to, conversationId);
                });
                response.body = {{"shared", result.shared}, {"pending", result.pending}};
            } else if (get && parts.size() == 6 && parts[3] == "actors" && parts[5] == "memories") {
                // Every memory of an actor, oldest first, for debugging and the host's own tests.
                const std::string actorId(parts[4]);
                nlohmann::json memories = nlohmann::json::array();
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    for (const core::Summary& memory : session.Memories(actorId)) {
                        memories.push_back({{"conversation_id", memory.conversationId},
                                            {"from_turn", memory.fromTurn},
                                            {"to_turn", memory.toTurn},
                                            {"text", memory.text},
                                            {"told_by", memory.toldBy},
                                            {"superseded", memory.superseded}});
                    }
                });
                response.body = {{"memories", memories}};
```

(Insert them so the existing `if / else if` chain stays one chain; check the branch order against the current code, which also routes `open` and `conversations` under `/v1/runs/{run_id}`. The `to` check of a missing field is `RequiredString`'s 400 "to: expected a string". `RunRegistry`'s 409 for an unopened run applies through `worker()`.)

Add the API 1.1.0 entry at the top of `CHANGELOG.md`:

```markdown
## API 1.1.0

Per-speaker memory (design: `docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md`). Only additions.

- `POST /v1/runs/{run_id}/memories/share` with `{from, to, conversation_id}` answers `{shared, pending}`: `to` learns
  what `from` remembers of the conversation, as copies marked told by `from`; a share waits (`pending`) until the
  conversation ended and `from`'s memory of it is complete.
- `GET /v1/runs/{run_id}/actors/{actor_id}/memories` answers `{memories: [{conversation_id, from_turn, to_turn, text,
  told_by, superseded}]}`, oldest first.
- Every NPC now remembers only what it witnessed; a turn's prompt shows the speaker's own memories.
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` three times in a row, then `cmake --build --preset asan` and `./build/asan/bin/Debug/morpheus_tests.exe "[hosted-server]"`.
Expected: `All tests passed` each time.

- [ ] **Step 5: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp CHANGELOG.md tests/server/
git commit -m "morpheus_server API 1.1.0: share an NPC's memories of a conversation, read an actor's memories; the three-actor fixture over HTTP"
```

---

### Task 8: Live runs and documentation

**Files:**
- Modify: `tests/hosted/HostedTestSupport.h` (the fixture runner takes its backend)
- Modify: `tests/llama/LlamaHostedTests.cpp`, `tests/server/ServerLiveTests.cpp`
- Modify: `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `CLAUDE.md` (only if a layout line changes)

**Interfaces:**
- Consumes: everything above.
- Produces: `test::PlayMemoryFixture(const std::string& name, inference::LlmBackend& backend, const core::Settings& settings, bool echoedMemories)`; the old one-argument form becomes a wrapper that makes the echo backend and calls it with `echoedMemories = true`.

- [ ] **Step 1: The runner takes its backend**

Split `PlayMemoryFixture` (Task 5, extended in Task 6): the four-argument form plays the fixture on `backend` with `settings`; with `echoedMemories` false (a real model writes the memories), it checks only what cannot depend on the model's words: every `prompt_absent`, every memories `absent` and `count`, the share's `pending`, and that each turn commits; it skips `prompt_contains`, memories `contains`, `told_by` and `shared`. The one-argument form keeps today's behaviour:

```cpp
inline void PlayMemoryFixture(const std::string& name)
{
    HostedScript script = ScriptOf(ReadFixture(name)); // The fixture's model replies, in step order.
    ScriptedBackend backend(16384, MemoryEchoResponder(script));
    PlayMemoryFixture(name, backend, HostedSettings(), true);
}
```

(`ScriptOf` is the loop that fills `script.actions` and `script.lines` from the steps, now a helper with two callers: this wrapper and the HTTP test, which should use it too.)

With a real model, a turn's line is the model's, so the fixture's `model` replies are ignored, and the substrings checked as absent (`"takes bribes"`, `"back to his shop"`) could appear only if a model wrote them unprompted where its NPC never heard them: a failure there is a real leak or a hallucination, and the test says which turn.

- [ ] **Step 2: The live tests**

In `tests/llama/LlamaHostedTests.cpp` (read it first: it has the in-process backend setup and skips without `MORPHEUS_TEST_CHAT_MODEL` / `MORPHEUS_TEST_EMBED_MODEL`), add:

```cpp
TEST_CASE("The three-actor fixture on the real model: nobody remembers what it did not witness", "[llama][hosted][memory]")
{
    llama::LlamaCppBackend& backend = test::SharedLlamaBackend(); // Use the file's own accessor for the shared backend.
    core::Settings settings = test::HostedSettings();
    settings.contextSize = backend.ContextSize();
    test::PlayMemoryFixture("three_actors.json", backend, settings, false);
}
```

(adapt the backend accessor to the file's helper). In `tests/server/ServerLiveTests.cpp`, add a `[server]` test that plays the same fixture over HTTP against the owner's llama-servers, like the HTTP test of Task 7 but on `LlamaServerBackend`s (the live test there shows how) and checking only what `echoedMemories = false` checks; extract the HTTP fixture loop of Task 7 into a helper in `tests/server/ServerTestSupport.h` taking the same flag so both tests share it.

Run: `cmake --build --preset tests`, then the fast suite (`ctest --preset tests`): the live tests skip without their variables.

If the owner's models are configured (`MORPHEUS_TEST_CHAT_MODEL`, `MORPHEUS_TEST_EMBED_MODEL`), build `dev` (`cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`) and run `"[llama][memory]"`; report the result. Do not start llama-servers yourself; the controller runs the `[server]` one.

- [ ] **Step 3: Documentation**

- README, morpheus_server section: the two routes in the endpoint table and the API version; a short paragraph on memory: each NPC remembers only what it witnessed (its own summaries, in its words, shown in its prompt's tail; retrieval limited to what it saw and was told); sharing; that the player has no memories in Morpheus.
- The architecture spec: in the module table's `turn` row, "per-speaker memory"; the `summaries` table row (`actor_id`, `conversation_id`, `told_by`, `source_id`, `superseded`), new rows for `actor_memory` and `pending_shares`; schema version 5; in "Hosted runs": summaries are per NPC (link this spec), the hosted prompt's memories are in the tail, and `MemoryUpkeep` works through a `SummaryScope`.
- The hosted-world spec: where it describes run-wide summaries (section 1 memory decision, the hosted prompt's L2, "Shared memory upkeep"), add one line each that E1 replaced them with per-NPC summaries, pointing to `2026-11-16-per-speaker-memory-design.md`.
- The morpheus_server spec: the two routes and API 1.1.0 in its endpoint table, marked "added in 1.1.0 (E1)".
- Check every changed doc with the byte check.

- [ ] **Step 4: Commit**

```bash
git add tests/hosted/HostedTestSupport.h tests/llama/LlamaHostedTests.cpp tests/server/ README.md docs/superpowers/specs/
git commit -m "Per-speaker memory: live runs of the three-actor fixture, and the docs"
```

---

## Final verification (controller)

- `ctest --preset tests` and `ctest --preset asan`: all pass; the `[llama]` and `[server]` tests skip without their variables.
- `cmake --build build/dev --config RelWithDebInfo`: clean; with the models configured, `"[llama][hosted]"` passes.
- With llama-servers running (Qwen3.5-9B and nomic-embed, as in the README): `"[server]"` passes, including the three-actor fixture over HTTP.
- The consumer check (README): ok.
- No `morpheus_tests`, `morpheus_server`, `llama-server` or `ctest` process left running.
