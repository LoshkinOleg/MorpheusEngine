### Task 2: RunStore schema version 3: run kind, hosted tables, the upgrade

The store learns hosted runs: `run_meta` gains `kind`; a hosted run keeps its host data (with the content hash in `world_hash`), its conversations, its turns and the events recorded between them. A version 2 file is upgraded in place on open; any other version is refused as today.

**Files:**
- Create: `src/core/HostedRecord.h`
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Create: `tests/store/HostedStoreTests.cpp`
- Modify: `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (`core/HostedRecord.h`, namespace `morpheus::core`): `struct Participant { std::string id; std::string name; std::string bio; }`; `struct Conversation { std::string id; std::vector<Participant> participants; bool ended = false; int foldedThrough = 0; }`; `struct ConversationEvent { int afterTurn = 0; nlohmann::json event; }`; `struct Adjudication { bool requested = false; std::string actionId; nlohmann::json params = object; std::vector<std::string> reasons; }`; `struct HostedTurn { int number; std::string conversationId; std::string speakerId; std::vector<std::string> listenerIds; std::optional<std::string> playerText; std::vector<nlohmann::json> events; nlohmann::json observation; std::string resolveOutput; Adjudication adjudication; std::string line; nlohmann::json timings; }`.
- Produces (`store/RunStore.h`): `enum class store::RunKind { Owned, Hosted }`; `RunMeta::kind` (default `Owned`); `RunStore::SCHEMA_VERSION == 3`; `static RunStore CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData)`; `nlohmann::json HostData()`; `void ReplaceHostData(const nlohmann::json& hostData, const std::string& hash)`; `std::string CreateConversation(const std::vector<core::Participant>& participants)` (ids `c1`, `c2`, ...); `std::optional<core::Conversation> Conversation(std::string_view id)`; `std::vector<core::Conversation> Conversations()` (creation order); `void SetParticipants(std::string_view id, const std::vector<core::Participant>& participants)`; `void EndConversation(std::string_view id)`; `void FoldConversation(std::string_view id, const core::Summary& summary)`; `void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events)`; `std::vector<core::ConversationEvent> ConversationEvents(std::string_view id)`; `int LatestHostedTurn()`; `void CommitHostedTurn(const core::HostedTurn& record)`; `std::vector<core::HostedTurn> HostedTurns(std::string_view conversationId)`. Every hosted method throws `ContractError` on an owned run.

- [ ] **Step 1: Write the failing tests**

Create `tests/store/HostedStoreTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
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

// The schema of a version 2 run exactly as RunStore wrote it before version 3.
constexpr const char* V2_SCHEMA_SQL = R"sql(
CREATE TABLE run_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    attempt   INTEGER NOT NULL,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    checks    TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    digest    TEXT NOT NULL,
    timings   TEXT NOT NULL
);
CREATE TABLE state_snapshots (
    turn  INTEGER PRIMARY KEY,
    state TEXT NOT NULL
);
CREATE TABLE summaries (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    from_turn INTEGER NOT NULL,
    to_turn   INTEGER NOT NULL,
    text      TEXT NOT NULL
);
CREATE TABLE notes (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    turn INTEGER NOT NULL,
    text TEXT NOT NULL
);
CREATE TABLE embeddings (
    kind         TEXT NOT NULL,
    ref_id       TEXT NOT NULL,
    model        TEXT NOT NULL,
    vector       BLOB NOT NULL,
    content_hash TEXT NOT NULL,
    PRIMARY KEY (kind, ref_id, model)
);
)sql";

store::RunMeta HostedMeta()
{
    store::RunMeta meta;
    meta.kind = store::RunKind::Hosted;
    meta.worldId = "hosted";
    meta.worldHash = "00000000000000aa";
    meta.modelId = "test-model";
    meta.rngSeed = 7;
    meta.settings = {{"context_size", 4096}};
    return meta;
}

nlohmann::json TestHostData()
{
    return nlohmann::json::parse(R"({"catalog": {"actions": [{"id": "none"}]},
        "observation_schema": {"type": "object", "properties": {}, "required": [], "additionalProperties": true},
        "lore": [{"subject": "inn", "data": "A warm inn.", "always_on": true}]})");
}

std::vector<core::Participant> TwoParticipants()
{
    return {{"npc-1", "Hulda", "Keeps the inn."}, {"player", "Aela", ""}};
}

core::HostedTurn TestHostedTurn(int number, const std::string& conversationId)
{
    core::HostedTurn turn;
    turn.number = number;
    turn.conversationId = conversationId;
    turn.speakerId = "npc-1";
    turn.listenerIds = {"player"};
    turn.playerText = "Follow me.";
    turn.events = {nlohmann::json{{"type", "ambient"}, {"text", "Rain starts."}}};
    turn.observation = {{"actors", nlohmann::json::array()}};
    turn.resolveOutput = R"({"action":"none","params":{}})";
    turn.adjudication = {true, "none", nlohmann::json::object(), {}};
    turn.line = std::format("Line {}.", number);
    turn.timings = {{"resolve", {{"prompt_tokens", 10}}}};
    return turn;
}

} // namespace

TEST_CASE("A hosted run stores its kind, host data, conversations, events and turns, and reopens with them",
          "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hosted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        CHECK(store.LatestHostedTurn() == 0);
        id = store.CreateConversation(TwoParticipants());
        CHECK(id == "c1");
        CHECK(store.CreateConversation({{"npc-2", "Saadia", ""}}) == "c2");
        store.AddConversationEvents(id, 0, {nlohmann::json{{"text", "A door opens."}},
                                            nlohmann::json{{"type", "ambient"}, {"text", "Rain."}}});
        store.CommitHostedTurn(TestHostedTurn(1, id));
        core::HostedTurn quiet = TestHostedTurn(2, id);
        quiet.playerText.reset();
        quiet.adjudication = {false, "", nlohmann::json::object(), {"/intent: unexpected key 'intent'"}};
        store.CommitHostedTurn(quiet);
        store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "Hulda leaves."}}});
    }

    store::RunStore reopened = store::RunStore::Open(file);
    CHECK(reopened.Meta().kind == store::RunKind::Hosted);
    CHECK(reopened.Meta().worldId == "hosted");
    CHECK(reopened.Meta().rngSeed == 7u);
    CHECK(reopened.HostData() == TestHostData());
    CHECK(reopened.LatestHostedTurn() == 2);
    CHECK(reopened.LatestTurn() == 0); // A hosted run plays no owned turns.

    const std::optional<core::Conversation> conversation = reopened.Conversation(id);
    REQUIRE(conversation);
    REQUIRE(conversation->participants.size() == 2);
    CHECK(conversation->participants[0].id == "npc-1");
    CHECK(conversation->participants[0].name == "Hulda");
    CHECK(conversation->participants[0].bio == "Keeps the inn.");
    CHECK(conversation->participants[1].bio.empty());
    CHECK_FALSE(conversation->ended);
    CHECK(conversation->foldedThrough == 0);
    CHECK_FALSE(reopened.Conversation("c9"));
    const std::vector<core::Conversation> all = reopened.Conversations();
    REQUIRE(all.size() == 2);
    CHECK(all[1].id == "c2");

    const std::vector<core::ConversationEvent> events = reopened.ConversationEvents(id);
    REQUIRE(events.size() == 3);
    CHECK(events[0].afterTurn == 0);
    CHECK(events[0].event == nlohmann::json{{"text", "A door opens."}});
    CHECK(events[1].event.at("type") == "ambient");
    CHECK(events[2].afterTurn == 2);
    CHECK(reopened.ConversationEvents("c2").empty());

    const std::vector<core::HostedTurn> turns = reopened.HostedTurns(id);
    REQUIRE(turns.size() == 2);
    const core::HostedTurn expected = TestHostedTurn(1, id);
    CHECK(turns[0].number == 1);
    CHECK(turns[0].conversationId == id);
    CHECK(turns[0].speakerId == "npc-1");
    CHECK(turns[0].listenerIds == std::vector<std::string>{"player"});
    CHECK(turns[0].playerText == std::optional<std::string>("Follow me."));
    CHECK(turns[0].events == expected.events);
    CHECK(turns[0].observation == expected.observation);
    CHECK(turns[0].resolveOutput == expected.resolveOutput);
    CHECK(turns[0].adjudication.requested);
    CHECK(turns[0].adjudication.actionId == "none");
    CHECK(turns[0].adjudication.params == nlohmann::json::object());
    CHECK(turns[0].line == "Line 1.");
    CHECK(turns[0].timings == expected.timings);
    CHECK_FALSE(turns[1].playerText);
    CHECK_FALSE(turns[1].adjudication.requested);
    CHECK(turns[1].adjudication.reasons == std::vector<std::string>{"/intent: unexpected key 'intent'"});
    CHECK(reopened.HostedTurns("c2").empty());
}

TEST_CASE("ReplaceHostData replaces the stored copy and the content hash", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hosted_replace") / "run.db";
    nlohmann::json changed = TestHostData();
    changed["catalog"]["actions"].push_back(nlohmann::json{{"id", "follow"}});
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        store.ReplaceHostData(changed, "00000000000000bb");
        CHECK(store.Meta().worldHash == "00000000000000bb");
        CHECK(store.HostData() == changed);
    }
    store::RunStore reopened = store::RunStore::Open(file);
    CHECK(reopened.Meta().worldHash == "00000000000000bb");
    CHECK(reopened.HostData() == changed);
}

TEST_CASE("FoldConversation adds the summary and moves folded_through together; participants and the end are kept",
          "[store][hosted]")
{
    store::RunStore store =
        store::RunStore::CreateHosted(test::FreshTempDir("store_hosted_fold") / "run.db", HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    store.FoldConversation(id, {1, 2, "They met."});
    CHECK(store.Conversation(id)->foldedThrough == 2);
    const std::vector<core::Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "They met.");
    CHECK_THROWS_AS(store.FoldConversation(id, {3, 2, "Backwards."}), core::ContractError);
    CHECK(store.Summaries().size() == 1);

    store.SetParticipants(id, {{"player", "Aela", ""}, {"npc-2", "Saadia", "Serves at the inn."}});
    const std::optional<core::Conversation> changed = store.Conversation(id);
    REQUIRE(changed);
    REQUIRE(changed->participants.size() == 2);
    CHECK(changed->participants[0].id == "player");
    CHECK(changed->participants[1].bio == "Serves at the inn.");
    CHECK_FALSE(changed->ended);
    store.EndConversation(id);
    CHECK(store.Conversation(id)->ended);
}

TEST_CASE("A version 2 run is upgraded in place to an owned version 3 run, its data unchanged", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_upgrade") / "run.db";
    {
        store::Database db = store::Database::Open(file, true);
        db.Exec(V2_SCHEMA_SQL);
        const std::vector<std::pair<std::string, std::string>> meta = {
            {"schema_version", "2"},  {"world_id", "mini"}, {"world_hash", "0123456789abcdef"},
            {"model_id", "old-model"}, {"rng_seed", "42"},  {"settings", R"({"context_size":8192})"},
            {"created_at", "2026-11-01T10:00:00Z"}};
        for (const auto& [key, value] : meta) {
            db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
        }
        db.Exec(R"sql(
INSERT INTO state_snapshots (turn, state) VALUES (0, '{"player":{"type":"character","fields":{"health":10}}}');
INSERT INTO state_snapshots (turn, state) VALUES (1, '{"player":{"type":"character","fields":{"health":9}}}');
INSERT INTO turns (number, attempt, input, resolve, checks, applied, rejected, narration, digest, timings)
    VALUES (1, 0, 'I climb.', '{"intent":"climb"}', '[]', '[]', '[]', 'You climb.', 'The player climbed.', '{}');
INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'A climb.');
INSERT INTO notes (turn, text) VALUES (1, 'The rope is frayed.');
)sql");
    }

    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Owned);
        CHECK(store.Meta().worldId == "mini");
        CHECK(store.Meta().worldHash == "0123456789abcdef");
        CHECK(store.Meta().rngSeed == 42u);
        CHECK(store.LatestTurn() == 1);
        const std::optional<core::TurnRecord> turn = store.Turn(1);
        REQUIRE(turn);
        CHECK(turn->input == "I climb.");
        CHECK(turn->narration == "You climb.");
        CHECK(turn->digest == "The player climbed.");
        CHECK(turn->notes == std::vector<std::string>{"The rope is frayed."});
        CHECK(store.StateAt(1).at("player").at("fields").at("health") == 9);
        const std::vector<core::Summary> summaries = store.Summaries();
        REQUIRE(summaries.size() == 1);
        CHECK(summaries[0].text == "A climb.");
    }

    // The file now says version 3 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "3");
        store::Statement kind = db.Prepare("SELECT value FROM run_meta WHERE key = 'kind'");
        REQUIRE(kind.Step());
        CHECK(kind.ColumnText(0) == "owned");
        store::Statement conversations = db.Prepare("SELECT COUNT(*) FROM conversations");
        REQUIRE(conversations.Step());
        CHECK(conversations.ColumnInt(0) == 0);
    }
    CHECK(store::RunStore::Open(file).LatestTurn() == 1);

    // A kind other than owned or hosted is damaged meta.
    {
        store::Database db = store::Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = 'alien' WHERE key = 'kind'");
    }
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run_meta 'kind'"));
}

TEST_CASE("Hosted methods refuse an owned run, a turn out of order and an unknown conversation", "[store][hosted]")
{
    const std::filesystem::path dir = test::FreshTempDir("store_hosted_refuse");
    store::RunMeta owned = HostedMeta();
    owned.kind = store::RunKind::Owned;
    owned.worldId = "mini";

    // Each kind is created by its own method.
    CHECK_THROWS_AS(store::RunStore::Create(dir / "x.db", HostedMeta(), nlohmann::json::object()), core::ContractError);
    CHECK_THROWS_AS(store::RunStore::CreateHosted(dir / "y.db", owned, TestHostData()), core::ContractError);
    CHECK_FALSE(std::filesystem::exists(dir / "x.db"));
    CHECK_FALSE(std::filesystem::exists(dir / "y.db"));

    store::RunStore ownedStore = store::RunStore::Create(dir / "owned.db", owned, nlohmann::json::object());
    CHECK(ownedStore.Meta().kind == store::RunKind::Owned);
    CHECK_THROWS_AS(ownedStore.CreateConversation(TwoParticipants()), core::ContractError);
    CHECK_THROWS_AS(ownedStore.HostData(), core::ContractError);
    CHECK_THROWS_AS(ownedStore.LatestHostedTurn(), core::ContractError);

    store::RunStore hosted = store::RunStore::CreateHosted(dir / "hosted.db", HostedMeta(), TestHostData());
    const std::string id = hosted.CreateConversation(TwoParticipants());
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(2, id)), core::ContractError);   // Not the next number.
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(1, "c9")), core::ContractError); // No such conversation.
    CHECK_THROWS_AS(hosted.CreateConversation({}), core::ContractError);
    CHECK_THROWS_AS(hosted.EndConversation("c9"), core::ContractError);
    CHECK_THROWS_AS(hosted.SetParticipants("c9", TwoParticipants()), core::ContractError);
    CHECK_THROWS_AS(hosted.AddConversationEvents("c9", 0, {nlohmann::json{{"text", "x"}}}), core::ContractError);
    CHECK(hosted.LatestHostedTurn() == 0);
}
```

In `tests/store/RunStoreTests.cpp`, replace the whole test case `RunStore refuses a version-1 run, naming the file and both versions` with:

```cpp
TEST_CASE("RunStore refuses a version-1 run, naming the file and both versions", "[store]")
{
    CHECK(RunStore::SCHEMA_VERSION == 3);
    const std::filesystem::path file = FreshDbPath("store_version_1");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '1' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_AS(RunStore::Open(file), StoreError);
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version 1 is not the supported version 3"));
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'core/HostedRecord.h'` in `tests/store/HostedStoreTests.cpp`.

- [ ] **Step 3: Add the hosted record types**

Create `src/core/HostedRecord.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// One participant of a hosted conversation, as it joined: the actor's id, with its name and bio from the observation
// it joined with. The participants layer of a hosted prompt shows them in join order.
struct Participant {
    std::string id = {};
    std::string name = {};
    std::string bio = {}; // Empty when the observation gave none.
};

// One conversation of a hosted run (hosted-world spec, section 5).
struct Conversation {
    std::string id = {};
    std::vector<Participant> participants = {}; // In join order.
    bool ended = false;
    int foldedThrough = 0; // This conversation's turns up to this number are folded into summaries; 0 when none are.
};

// An event recorded between turns (turn::HostedSession::RecordEvents): the host's JSON object, with its `text`.
struct ConversationEvent {
    int afterTurn = 0; // The conversation's latest turn when it was recorded; 0 before its first.
    nlohmann::json event = nlohmann::json::object();
};

// What became of the speaker's proposed action (turn::HostedWorld::Adjudicate). A requested action is the host's to
// carry out; its outcome comes back later as an event. A refused one has the parse or validation errors as reasons,
// and the speaker takes no action.
struct Adjudication {
    bool requested = false;
    std::string actionId = {};
    nlohmann::json params = nlohmann::json::object();
    std::vector<std::string> reasons = {};
};

// One committed hosted turn (hosted-world spec, section 2, step 4). Stored by store::RunStore; rendered into a hosted
// prompt's L3 by turn::HostedSession.
struct HostedTurn {
    int number = 0; // Run-wide, shared with summaries' turn ranges.
    std::string conversationId = {};
    std::string speakerId = {};
    std::vector<std::string> listenerIds = {};       // The other participants present, in join order.
    std::optional<std::string> playerText = {};      // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};         // The events this turn's request added, in order.
    nlohmann::json observation = nlohmann::json::object();
    std::string resolveOutput = {};                  // The resolve call's text exactly as the model wrote it.
    Adjudication adjudication = {};
    std::string line = {};                           // The spoken line, trimmed.
    nlohmann::json timings = nlohmann::json::object();
};

} // namespace morpheus::core
```

- [ ] **Step 4: Move the store to version 3**

Replace the whole of `src/store/RunStore.h` with:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/TurnRecord.h"
#include "store/Sqlite.h"

namespace morpheus::store {

// Owned: Morpheus's own game, with its world, state and turns. Hosted: a world the host owns and sends as data
// (turn::HostedSession), with conversations and hosted turns.
enum class RunKind { Owned, Hosted };

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation; a hosted run's host data hash, kept current.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
    RunKind kind = RunKind::Owned;
};

enum class EmbeddingKind { Lore, Turn, Note };

// refId: lore subject for Lore, decimal turn number for Turn, "<turn>.<index>" for Note (index: the note's
// position among that turn's notes).
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
    std::string contentHash = {}; // Hash of the exact text that was embedded. Callers re-embed when it differs
                                  // from the hash of the current text, so edited lore never keeps a stale vector.
};

// One run's SQLite database (saves/<world>/<run>.db). The only writer of run data.
// Tables: run_meta, turns, state_snapshots, summaries, notes, embeddings, and for hosted runs host_data,
// conversations, hosted_turns, conversation_events. Every file has every table; summaries and embeddings serve both
// kinds, and turn numbers run across the whole run.
class RunStore {
public:
    // 3: run_meta.kind and the hosted tables. 2: turns.digest (it also covers turns.attempt). A version 2 run is
    // upgraded in place on open; any other version is refused.
    static constexpr int SCHEMA_VERSION = 3;

private:
    Database _db;
    RunMeta _meta = {};
    std::filesystem::path _file = {}; // Only used to name the file in error messages.

public:
    // Creates a new owned run database holding the meta and the turn-0 snapshot. Throws StoreError if the file
    // exists, and ContractError unless meta.kind is Owned. If creation fails part way, the partial file is removed.
    static RunStore Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState);

    // Creates a new hosted run database holding the meta and the host data (turn::HostDataToJson). Throws StoreError
    // if the file exists, and ContractError unless meta.kind is Hosted. If creation fails part way, the partial file
    // is removed.
    static RunStore CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData);

    // Opens an existing run. A version 2 file is first upgraded in place to version 3 (the hosted tables are added
    // and it is marked owned, in one transaction; its data is not touched). Throws StoreError if it is missing or was
    // written with another schema version, or its meta (rng_seed, settings, kind) is damaged.
    static RunStore Open(const std::filesystem::path& file);

    const RunMeta& Meta() const;

    // Highest committed turn number, 0 before the first turn.
    int LatestTurn();

    // State after `turn` (0 = initial state). Throws ContractError if there is no snapshot for it
    // and StoreError if the stored snapshot is not valid JSON.
    nlohmann::json StateAt(int turn);

    // Appends turn LatestTurn()+1, the state it produced and the notes it wrote (record.notes, stored with
    // turn = record.number), in one transaction: either all of it commits or none of it does.
    // Throws ContractError if record.number is not LatestTurn()+1.
    void CommitTurn(const core::TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist. record.notes holds that turn's notes in the order they were written.
    // Throws StoreError if a stored column is not valid JSON or has the wrong shape, or a note has no matching
    // turn row.
    std::optional<core::TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending, each with its notes. Throws StoreError if a stored column is not
    // valid JSON or has the wrong shape, or a note has no matching turn row.
    std::vector<core::TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn and note embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);

    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const core::Summary& summary);

    // Condensing: replaces every summary with `merged` in one transaction. Throws ContractError unless
    // 1 <= merged.fromTurn <= merged.toTurn.
    void ReplaceSummaries(const core::Summary& merged);

    // Ordered by fromTurn.
    std::vector<core::Summary> Summaries();

    // Every committed note, ordered by turn, then the order written. For context assembly in assisted mode.
    std::vector<core::Note> Notes();

    // Inserts or replaces the vector and content hash for (kind, refId, model).
    // Throws ContractError on an empty vector or an empty contentHash.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model, with the content hash it was stored with.
    // Ordered by the number refId starts with, then refId.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);

    // Hosted runs only: every method below throws ContractError on an owned run.

    // The host data as stored. Throws StoreError when it is missing or not valid JSON.
    nlohmann::json HostData();

    // A mod update: replaces the stored host data and its content hash (run_meta world_hash, and Meta().worldHash)
    // together. Throws ContractError for an empty hash.
    void ReplaceHostData(const nlohmann::json& hostData, const std::string& hash);

    // Starts a conversation with `participants` in join order and returns its id: "c1", "c2", ... in creation order.
    // Throws ContractError for no participants.
    std::string CreateConversation(const std::vector<core::Participant>& participants);

    // nullopt when there is no conversation `id`. Throws StoreError when its participants column is damaged.
    std::optional<core::Conversation> Conversation(std::string_view id);

    // Every conversation, in creation order. Throws StoreError when a participants column is damaged.
    std::vector<core::Conversation> Conversations();

    // Replaces the participants of conversation `id`. Throws ContractError for an unknown conversation or none.
    void SetParticipants(std::string_view id, const std::vector<core::Participant>& participants);

    // Marks conversation `id` ended. Throws ContractError for an unknown conversation.
    void EndConversation(std::string_view id);

    // Adds `summary` and sets conversation `id`'s folded_through to summary.toTurn, in one transaction. Throws
    // ContractError for an unknown conversation or unless 1 <= fromTurn <= toTurn.
    void FoldConversation(std::string_view id, const core::Summary& summary);

    // Appends `events` to conversation `id`, in order, each with `afterTurn` (the conversation's latest turn, 0 before
    // its first), in one transaction. Throws ContractError for an unknown conversation.
    void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events);

    // Conversation `id`'s events in the order they were recorded. Throws StoreError when one is not valid JSON.
    std::vector<core::ConversationEvent> ConversationEvents(std::string_view id);

    // Highest committed hosted turn number, 0 before the first.
    int LatestHostedTurn();

    // Appends hosted turn LatestHostedTurn()+1. Throws ContractError if record.number is not that, or its
    // conversation is unknown.
    void CommitHostedTurn(const core::HostedTurn& record);

    // Conversation `conversationId`'s turns, ascending. Throws StoreError if a stored column is not valid JSON or has
    // the wrong shape.
    std::vector<core::HostedTurn> HostedTurns(std::string_view conversationId);

private:
    RunStore(Database db, RunMeta meta, std::filesystem::path file);

    // Create and CreateHosted: makes the file, its whole schema and the meta rows, then runs `fill` in the same
    // transaction. A failure part way removes the file.
    static RunStore CreateWith(const std::filesystem::path& file, RunMeta meta,
                               const std::function<void(Database& db)>& fill);

    // Throws ContractError naming `method` unless this is a hosted run.
    void RequireHosted(const char* method) const;
};

} // namespace morpheus::store
```

In `src/store/RunStore.cpp`, make these changes.

1. Replace the include block

```cpp
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>
```

with

```cpp
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>
#include <vector>
```

2. After the closing `)sql";` of `SCHEMA_SQL` (and before `// Stored spelling of an embedding kind.`), insert:

```cpp

// The tables version 3 adds: a hosted run's host data, its conversations, its turns and the events recorded between
// them. Created in every file, so a version 2 file is upgraded by adding them.
constexpr const char* HOSTED_SCHEMA_SQL = R"sql(
CREATE TABLE host_data (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    data TEXT NOT NULL
);
CREATE TABLE conversations (
    id             TEXT PRIMARY KEY,
    position       INTEGER NOT NULL UNIQUE,
    participants   TEXT NOT NULL,
    ended          INTEGER NOT NULL,
    folded_through INTEGER NOT NULL
);
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
    timings         TEXT NOT NULL
);
CREATE TABLE conversation_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id TEXT NOT NULL,
    after_turn      INTEGER NOT NULL,
    event           TEXT NOT NULL
);
)sql";

// Stored spelling of a run kind, run_meta 'kind'.
const char* RunKindName(RunKind kind)
{
    switch (kind) {
    case RunKind::Owned:
        return "owned";
    case RunKind::Hosted:
        return "hosted";
    }
    throw morpheus::core::ContractError("RunKindName: unhandled RunKind");
}

// A conversation's participants as stored: [{"id": ..., "name": ..., "bio": ...}] in join order.
nlohmann::json ParticipantsToJson(const std::vector<core::Participant>& participants)
{
    nlohmann::json json = nlohmann::json::array();
    for (const core::Participant& participant : participants) {
        json.push_back({{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
    }
    return json;
}
```

3. Replace the whole function `RunStore RunStore::Create(...)` (from `RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)` to its closing brace) with:

```cpp
RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)
{
    MORPHEUS_REQUIRE(meta.kind == RunKind::Owned, "RunStore::Create: a hosted run is created with CreateHosted");
    // An owned run starts from its world's initial state.
    return CreateWith(file, std::move(meta), [&initialState](Database& db) {
        db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (0, ?1)").Bind(1, initialState.dump()).Run();
    });
}

RunStore RunStore::CreateHosted(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& hostData)
{
    MORPHEUS_REQUIRE(meta.kind == RunKind::Hosted, "RunStore::CreateHosted: an owned run is created with Create");
    // A hosted run has no state: it starts with the host's data.
    return CreateWith(file, std::move(meta), [&hostData](Database& db) {
        db.Prepare("INSERT INTO host_data (id, data) VALUES (1, ?1)").Bind(1, hostData.dump()).Run();
    });
}

RunStore RunStore::CreateWith(const std::filesystem::path& file, RunMeta meta,
                              const std::function<void(Database& db)>& fill)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    if (exists) {
        throw StoreError(std::format("run database '{}' already exists", core::PathText(file)));
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            throw StoreError(
                std::format("cannot create the folder for '{}': {}", core::PathText(file), error.message()));
        }
    }
    meta.createdAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                 std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Opening creates the (empty) file, so any failure from here on must remove it again, or a retry would be
    // blocked by "already exists".
    try {
        // Schema, meta and the kind's first rows land together or not at all.
        Database db = Database::Open(file, true);
        db.Transaction([&] {
            db.Exec(SCHEMA_SQL);
            db.Exec(HOSTED_SCHEMA_SQL);

            const std::array<std::pair<const char*, std::string>, 8> metaRows = {{
                {"schema_version", std::to_string(SCHEMA_VERSION)},
                {"world_id", meta.worldId},
                {"world_hash", meta.worldHash},
                {"model_id", meta.modelId},
                {"rng_seed", std::to_string(meta.rngSeed)}, // Text: SQLite integers are signed 64-bit.
                {"settings", meta.settings.dump()},
                {"created_at", meta.createdAt},
                {"kind", RunKindName(meta.kind)},
            }};
            for (const auto& [key, value] : metaRows) {
                db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
            }
            fill(db);
        });
        return RunStore(std::move(db), std::move(meta), file);
    } catch (...) {
        // `db` is already closed here (unwinding destroyed it), which Windows requires before deleting the file.
        // The non-throwing overload keeps the original error as the one that propagates.
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        throw;
    }
}
```

4. In `RunStore::Open`, replace

```cpp
    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     core::PathText(file), value("schema_version"), SCHEMA_VERSION));
    }
```

with

```cpp
    // A version 2 run is upgraded in place: the hosted tables are added and it is marked owned, in one transaction.
    // Its data is not touched. Any other version is refused.
    if (value("schema_version") == "2") {
        db.Transaction([&] {
            db.Exec(HOSTED_SCHEMA_SQL);
            db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)").Bind(1, RunKindName(RunKind::Owned)).Run();
            db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'schema_version'")
                .Bind(1, std::to_string(SCHEMA_VERSION))
                .Run();
        });
        values["schema_version"] = std::to_string(SCHEMA_VERSION);
        values["kind"] = RunKindName(RunKind::Owned);
    }
    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     core::PathText(file), value("schema_version"), SCHEMA_VERSION));
    }
```

and replace

```cpp
    meta.createdAt = value("created_at");
    return RunStore(std::move(db), std::move(meta), file);
}
```

with

```cpp
    meta.createdAt = value("created_at");

    const std::string& kindText = value("kind");
    if (kindText == RunKindName(RunKind::Owned)) {
        meta.kind = RunKind::Owned;
    } else if (kindText == RunKindName(RunKind::Hosted)) {
        meta.kind = RunKind::Hosted;
    } else {
        throw StoreError(std::format("'{}': run_meta 'kind' is '{}', not 'owned' or 'hosted'", core::PathText(file),
                                     kindText));
    }
    return RunStore(std::move(db), std::move(meta), file);
}
```

5. Before the constructor definition `RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)` at the end of the file, insert the hosted methods:

```cpp
nlohmann::json RunStore::HostData()
{
    RequireHosted("HostData");
    Statement query = _db.Prepare("SELECT data FROM host_data WHERE id = 1");
    if (!query.Step()) {
        throw StoreError(std::format("'{}': the hosted run has no host data", core::PathText(_file)));
    }
    try {
        return nlohmann::json::parse(query.ColumnText(0));
    } catch (const nlohmann::json::parse_error& error) {
        throw StoreError(std::format("'{}': host data is not valid JSON: {}", core::PathText(_file), error.what()));
    }
}

void RunStore::ReplaceHostData(const nlohmann::json& hostData, const std::string& hash)
{
    RequireHosted("ReplaceHostData");
    MORPHEUS_REQUIRE(!hash.empty(), "ReplaceHostData: the content hash must not be empty");
    // The data and its hash change together, so a reopen never compares new data with an old hash.
    _db.Transaction([&] {
        _db.Prepare("UPDATE host_data SET data = ?1 WHERE id = 1").Bind(1, hostData.dump()).Run();
        _db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'world_hash'").Bind(1, hash).Run();
    });
    _meta.worldHash = hash;
}

std::string RunStore::CreateConversation(const std::vector<core::Participant>& participants)
{
    RequireHosted("CreateConversation");
    MORPHEUS_REQUIRE(!participants.empty(), "CreateConversation: a conversation needs a participant");
    // Conversations are never deleted, so their count names the next one: c1, c2, ... The count's statement is
    // finished before the insert.
    std::int64_t position = 0;
    {
        Statement count = _db.Prepare("SELECT COUNT(*) FROM conversations");
        const bool hasRow = count.Step();
        MORPHEUS_REQUIRE(hasRow, "CreateConversation: aggregate query returned no row");
        position = count.ColumnInt(0) + 1;
    }
    const std::string id = std::format("c{}", position);
    _db.Prepare("INSERT INTO conversations (id, position, participants, ended, folded_through) "
                "VALUES (?1, ?2, ?3, 0, 0)")
        .Bind(1, id)
        .Bind(2, position)
        .Bind(3, ParticipantsToJson(participants).dump())
        .Run();
    return id;
}

std::optional<core::Conversation> RunStore::Conversation(std::string_view id)
{
    RequireHosted("Conversation");
    for (core::Conversation& conversation : Conversations()) {
        if (conversation.id == id) {
            return std::move(conversation);
        }
    }
    return std::nullopt;
}

std::vector<core::Conversation> RunStore::Conversations()
{
    RequireHosted("Conversations");
    Statement query =
        _db.Prepare("SELECT id, participants, ended, folded_through FROM conversations ORDER BY position");
    std::vector<core::Conversation> conversations;
    while (query.Step()) {
        core::Conversation conversation;
        conversation.id = query.ColumnText(0);
        // Damaged participants are a StoreError naming the file and the conversation.
        auto damaged = [&](const std::string& problem) {
            return StoreError(std::format("'{}': conversation '{}' column 'participants' {}", core::PathText(_file),
                                          conversation.id, problem));
        };
        nlohmann::json participants;
        try {
            participants = nlohmann::json::parse(query.ColumnText(1));
        } catch (const nlohmann::json::parse_error& error) {
            throw damaged(std::string("is not valid JSON: ") + error.what());
        }
        if (!participants.is_array()) {
            throw damaged("is not an array");
        }
        for (const nlohmann::json& participant : participants) {
            const bool whole = participant.is_object() && participant.contains("id") && participant.at("id").is_string() &&
                               participant.contains("name") && participant.at("name").is_string() &&
                               participant.contains("bio") && participant.at("bio").is_string();
            if (!whole) {
                throw damaged("holds a participant without a string id, name and bio");
            }
            conversation.participants.push_back({participant.at("id").get<std::string>(),
                                                 participant.at("name").get<std::string>(),
                                                 participant.at("bio").get<std::string>()});
        }
        conversation.ended = query.ColumnInt(2) != 0;
        conversation.foldedThrough = static_cast<int>(query.ColumnInt(3));
        conversations.push_back(std::move(conversation));
    }
    return conversations;
}

void RunStore::SetParticipants(std::string_view id, const std::vector<core::Participant>& participants)
{
    RequireHosted("SetParticipants");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "SetParticipants: no such conversation");
    MORPHEUS_REQUIRE(!participants.empty(), "SetParticipants: a conversation needs a participant");
    _db.Prepare("UPDATE conversations SET participants = ?1 WHERE id = ?2")
        .Bind(1, ParticipantsToJson(participants).dump())
        .Bind(2, id)
        .Run();
}

void RunStore::EndConversation(std::string_view id)
{
    RequireHosted("EndConversation");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "EndConversation: no such conversation");
    _db.Prepare("UPDATE conversations SET ended = 1 WHERE id = ?1").Bind(1, id).Run();
}

void RunStore::FoldConversation(std::string_view id, const core::Summary& summary)
{
    RequireHosted("FoldConversation");
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("FoldConversation: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "FoldConversation: no such conversation");
    // The summary and the conversation's folded mark land together: a turn is never both folded and shown in L3.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
            .Bind(1, summary.fromTurn)
            .Bind(2, summary.toTurn)
            .Bind(3, summary.text)
            .Run();
        _db.Prepare("UPDATE conversations SET folded_through = ?1 WHERE id = ?2").Bind(1, summary.toTurn).Bind(2, id).Run();
    });
}

void RunStore::AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events)
{
    RequireHosted("AddConversationEvents");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "AddConversationEvents: no such conversation");
    _db.Transaction([&] {
        for (const nlohmann::json& event : events) {
            _db.Prepare("INSERT INTO conversation_events (conversation_id, after_turn, event) VALUES (?1, ?2, ?3)")
                .Bind(1, id)
                .Bind(2, afterTurn)
                .Bind(3, event.dump())
                .Run();
        }
    });
}

std::vector<core::ConversationEvent> RunStore::ConversationEvents(std::string_view id)
{
    RequireHosted("ConversationEvents");
    Statement query =
        _db.Prepare("SELECT after_turn, event FROM conversation_events WHERE conversation_id = ?1 ORDER BY id");
    query.Bind(1, id);
    std::vector<core::ConversationEvent> events;
    while (query.Step()) {
        core::ConversationEvent event;
        event.afterTurn = static_cast<int>(query.ColumnInt(0));
        try {
            event.event = nlohmann::json::parse(query.ColumnText(1));
        } catch (const nlohmann::json::parse_error& error) {
            throw StoreError(std::format("'{}': an event of conversation '{}' is not valid JSON: {}",
                                         core::PathText(_file), id, error.what()));
        }
        events.push_back(std::move(event));
    }
    return events;
}

int RunStore::LatestHostedTurn()
{
    RequireHosted("LatestHostedTurn");
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM hosted_turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestHostedTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

void RunStore::CommitHostedTurn(const core::HostedTurn& record)
{
    RequireHosted("CommitHostedTurn");
    const int expected = LatestHostedTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitHostedTurn: expected turn {}, got {}", expected, record.number));
    MORPHEUS_REQUIRE(Conversation(record.conversationId).has_value(), "CommitHostedTurn: no such conversation");

    // JSON columns. The replace handler keeps a reason quoting text that is not UTF-8 from throwing.
    const nlohmann::json listeners = record.listenerIds;
    const nlohmann::json playerText = record.playerText ? nlohmann::json(*record.playerText) : nlohmann::json(nullptr);
    nlohmann::json events = nlohmann::json::array();
    for (const nlohmann::json& event : record.events) {
        events.push_back(event);
    }
    const nlohmann::json adjudication = {{"requested", record.adjudication.requested},
                                         {"action_id", record.adjudication.actionId},
                                         {"params", record.adjudication.params},
                                         {"reasons", record.adjudication.reasons}};
    _db.Prepare("INSERT INTO hosted_turns (number, conversation_id, speaker_id, listener_ids, player_text, events, "
                "observation, resolve_output, adjudication, line, timings) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)")
        .Bind(1, record.number)
        .Bind(2, record.conversationId)
        .Bind(3, record.speakerId)
        .Bind(4, listeners.dump())
        .Bind(5, playerText.dump())
        .Bind(6, events.dump())
        .Bind(7, record.observation.dump())
        .Bind(8, record.resolveOutput)
        .Bind(9, adjudication.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
        .Bind(10, record.line)
        .Bind(11, record.timings.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
        .Run();
}

std::vector<core::HostedTurn> RunStore::HostedTurns(std::string_view conversationId)
{
    RequireHosted("HostedTurns");
    Statement query = _db.Prepare("SELECT number, speaker_id, listener_ids, player_text, events, observation, "
                                  "resolve_output, adjudication, line, timings FROM hosted_turns "
                                  "WHERE conversation_id = ?1 ORDER BY number");
    query.Bind(1, conversationId);
    std::vector<core::HostedTurn> turns;
    while (query.Step()) {
        core::HostedTurn turn;
        turn.number = static_cast<int>(query.ColumnInt(0));
        turn.conversationId = std::string(conversationId);
        // Damaged saved data is a StoreError naming the file, the turn and the column.
        auto damaged = [&](const char* column, const std::string& problem) {
            return StoreError(std::format("'{}': hosted turn {} column '{}' {}", core::PathText(_file), turn.number,
                                          column, problem));
        };
        auto parseColumn = [&](int column, const char* name) {
            try {
                return nlohmann::json::parse(query.ColumnText(column));
            } catch (const nlohmann::json::parse_error& error) {
                throw damaged(name, std::string("is not valid JSON: ") + error.what());
            }
        };
        turn.speakerId = query.ColumnText(1);

        const nlohmann::json listeners = parseColumn(2, "listener_ids");
        if (!listeners.is_array()) {
            throw damaged("listener_ids", "is not an array");
        }
        for (const nlohmann::json& listener : listeners) {
            if (!listener.is_string()) {
                throw damaged("listener_ids", "holds a value that is not a string");
            }
            turn.listenerIds.push_back(listener.get<std::string>());
        }

        const nlohmann::json playerText = parseColumn(3, "player_text");
        if (playerText.is_string()) {
            turn.playerText = playerText.get<std::string>();
        } else if (!playerText.is_null()) {
            throw damaged("player_text", "is neither a string nor null");
        }

        const nlohmann::json events = parseColumn(4, "events");
        if (!events.is_array()) {
            throw damaged("events", "is not an array");
        }
        for (const nlohmann::json& event : events) {
            turn.events.push_back(event);
        }
        turn.observation = parseColumn(5, "observation");
        turn.resolveOutput = query.ColumnText(6);

        const nlohmann::json adjudication = parseColumn(7, "adjudication");
        const bool whole = adjudication.is_object() && adjudication.contains("requested") &&
                           adjudication.at("requested").is_boolean() && adjudication.contains("action_id") &&
                           adjudication.at("action_id").is_string() && adjudication.contains("params") &&
                           adjudication.contains("reasons") && adjudication.at("reasons").is_array();
        if (!whole) {
            throw damaged("adjudication", "is malformed");
        }
        turn.adjudication.requested = adjudication.at("requested").get<bool>();
        turn.adjudication.actionId = adjudication.at("action_id").get<std::string>();
        turn.adjudication.params = adjudication.at("params");
        for (const nlohmann::json& reason : adjudication.at("reasons")) {
            if (!reason.is_string()) {
                throw damaged("adjudication", "holds a reason that is not a string");
            }
            turn.adjudication.reasons.push_back(reason.get<std::string>());
        }

        turn.line = query.ColumnText(8);
        turn.timings = parseColumn(9, "timings");
        turns.push_back(std::move(turn));
    }
    return turns;
}

void RunStore::RequireHosted(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Hosted, std::format("RunStore::{}: this is an owned run", method));
}

```

- [ ] **Step 5: Run the store tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: builds with no warnings; `All tests passed`.

- [ ] **Step 6: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 439` (427 + 7 + 5), the three `[server]` tests "Not Run (Skipped)". `Session` and the app open their runs unchanged: new owned runs are version 3 with kind `owned`.

- [ ] **Step 7: Byte check and commit**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
git add src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp tests/store/RunStoreTests.cpp
git commit -m "RunStore schema version 3: run kind, hosted tables and methods, in-place upgrade of version 2" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

