#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
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

// The schema of a version 5 run exactly as RunStore wrote it before version 6: SCHEMA_SQL, HOSTED_SCHEMA_SQL and
// PER_ACTOR_TABLES_SQL.
constexpr const char* V5_SCHEMA_SQL = R"sql(
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
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    from_turn       INTEGER NOT NULL,
    to_turn         INTEGER NOT NULL,
    text            TEXT NOT NULL,
    actor_id        TEXT NOT NULL DEFAULT '',
    conversation_id TEXT NOT NULL DEFAULT '',
    told_by         TEXT NOT NULL DEFAULT '',
    source_id       INTEGER NOT NULL DEFAULT 0,
    superseded      INTEGER NOT NULL DEFAULT 0
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
    timings         TEXT NOT NULL,
    events_omitted  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE conversation_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id TEXT NOT NULL,
    after_turn      INTEGER NOT NULL,
    event           TEXT NOT NULL
);
CREATE INDEX hosted_turns_conversation ON hosted_turns (conversation_id);
CREATE INDEX conversation_events_conversation ON conversation_events (conversation_id);
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
    turn.observation = nlohmann::json::parse(R"({"actors": [{"id": "npc-1", "name": "Hulda", "is_player": false},
                                                            {"id": "player", "name": "Aela", "is_player": true}]})");
    turn.resolveOutput = R"({"action":"none","params":{}})";
    turn.adjudication = {true, "none", nlohmann::json::object(), {}};
    turn.line = std::format("Line {}.", number);
    turn.timings = {{"resolve", {{"prompt_tokens", 10}}}};
    return turn;
}

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

TEST_CASE("A hosted run stores its kind, host data, conversations, events and turns, and reopens with them",
          "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hosted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        CHECK(store.LatestHostedTurn() == 0);
        id = store.CreateConversation(TwoParticipants(), std::nullopt);
        CHECK(id == "c1");
        CHECK(store.CreateConversation({{"npc-2", "Saadia", ""}}, std::nullopt) == "c2");
        store.AddConversationEvents(id, 0,
                                    {nlohmann::json{{"text", "A door opens."}},
                                     nlohmann::json{{"type", "ambient"}, {"text", "Rain."}}},
                                    std::nullopt);
        store.CommitHostedTurn(TestHostedTurn(1, id));
        core::HostedTurn quiet = TestHostedTurn(2, id);
        quiet.playerText.reset();
        quiet.adjudication = {false, "", nlohmann::json::object(), {"/intent: unexpected key 'intent'"}};
        store.CommitHostedTurn(quiet);
        store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "Hulda leaves."}}}, std::nullopt);
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
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    store.FoldConversation(id, ActorSummary("npc-1", id, 1, 2, "They met."));
    CHECK(store.Conversation(id)->foldedThrough == 2);
    const std::vector<core::Summary> summaries = store.ActorSummaries("npc-1", true);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "They met.");
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", id, 3, 2, "Backwards.")), core::ContractError);
    CHECK(store.HostedSummaries().size() == 1);

    store.SetParticipants(id, {{"player", "Aela", ""}, {"npc-2", "Saadia", "Serves at the inn."}}, std::nullopt);
    const std::optional<core::Conversation> changed = store.Conversation(id);
    REQUIRE(changed);
    REQUIRE(changed->participants.size() == 2);
    CHECK(changed->participants[0].id == "player");
    CHECK(changed->participants[1].bio == "Serves at the inn.");
    CHECK_FALSE(changed->ended);
    store.EndConversation(id, std::nullopt);
    CHECK(store.Conversation(id)->ended);
}

TEST_CASE("A version 2 run is upgraded in place to an owned version 6 run, its data unchanged", "[store][hosted]")
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

    // The file now says version 6 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "6");
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
    CHECK_THROWS_AS(ownedStore.CreateConversation(TwoParticipants(), std::nullopt), core::ContractError);
    CHECK_THROWS_AS(ownedStore.HostData(), core::ContractError);
    CHECK_THROWS_AS(ownedStore.LatestHostedTurn(), core::ContractError);

    store::RunStore hosted = store::RunStore::CreateHosted(dir / "hosted.db", HostedMeta(), TestHostData());
    const std::string id = hosted.CreateConversation(TwoParticipants(), std::nullopt);
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(2, id)), core::ContractError);   // Not the next number.
    CHECK_THROWS_AS(hosted.CommitHostedTurn(TestHostedTurn(1, "c9")), core::ContractError); // No such conversation.
    CHECK_THROWS_AS(hosted.CreateConversation({}, std::nullopt), core::ContractError);
    CHECK_THROWS_AS(hosted.EndConversation("c9", std::nullopt), core::ContractError);
    CHECK_THROWS_AS(hosted.SetParticipants("c9", TwoParticipants(), std::nullopt), core::ContractError);
    CHECK_THROWS_AS(hosted.AddConversationEvents("c9", 0, {nlohmann::json{{"text", "x"}}}, std::nullopt),
                    core::ContractError);
    CHECK(hosted.LatestHostedTurn() == 0);
}

namespace {

// Takes back what version 6 added to a file (clocks and hidden marks, conversations as history, summary supersessions
// and made_in, applied shares and their applied flag, rollbacks), to make a version 5 file from a new one: each
// conversation's latest change row becomes its participants and ended columns again, and a summary with a condense is
// superseded. The caller marks the version.
constexpr const char* DROP_VERSION_6_SQL =
    "ALTER TABLE summaries ADD COLUMN superseded INTEGER NOT NULL DEFAULT 0;"
    "UPDATE summaries SET superseded = 1 WHERE id IN (SELECT summary_id FROM summary_supersessions);"
    "DROP TABLE summary_supersessions;"
    "ALTER TABLE conversations ADD COLUMN participants TEXT NOT NULL DEFAULT '[]';"
    "ALTER TABLE conversations ADD COLUMN ended INTEGER NOT NULL DEFAULT 0;"
    "UPDATE conversations SET"
    " participants = (SELECT c.participants FROM conversation_changes c WHERE c.conversation_id = conversations.id"
    "                 ORDER BY c.id DESC LIMIT 1),"
    " ended = (SELECT c.ended FROM conversation_changes c WHERE c.conversation_id = conversations.id"
    "          ORDER BY c.id DESC LIMIT 1);"
    "DROP INDEX conversation_changes_conversation; DROP TABLE conversation_changes; DROP TABLE rollbacks;"
    "ALTER TABLE hosted_turns DROP COLUMN clock; ALTER TABLE hosted_turns DROP COLUMN hidden_by;"
    "ALTER TABLE conversation_events DROP COLUMN clock; ALTER TABLE conversation_events DROP COLUMN hidden_by;"
    "ALTER TABLE conversations DROP COLUMN clock; ALTER TABLE conversations DROP COLUMN hidden_by;"
    "ALTER TABLE summaries DROP COLUMN clock; ALTER TABLE summaries DROP COLUMN hidden_by;"
    "ALTER TABLE summaries DROP COLUMN made_in;"
    "ALTER TABLE pending_shares DROP COLUMN clock; ALTER TABLE pending_shares DROP COLUMN applied_clock;"
    "ALTER TABLE pending_shares DROP COLUMN unapplied_by; ALTER TABLE pending_shares DROP COLUMN hidden_by;"
    "ALTER TABLE pending_shares DROP COLUMN applied;";

// Takes back what version 5 added to a file (the summaries' per-actor columns and the per-speaker memory tables), to
// make an older file from a new one. The caller marks the version.
constexpr const char* DROP_VERSION_5_SQL = "DROP INDEX summaries_actor;" // An indexed column cannot be dropped.
                                           "DROP TABLE actor_memory; DROP TABLE pending_shares;"
                                           "ALTER TABLE summaries DROP COLUMN actor_id;"
                                           "ALTER TABLE summaries DROP COLUMN conversation_id;"
                                           "ALTER TABLE summaries DROP COLUMN told_by;"
                                           "ALTER TABLE summaries DROP COLUMN source_id;"
                                           "ALTER TABLE summaries DROP COLUMN superseded;";

// Runs raw SQL against a closed run file, to damage what the store saved.
void Damage(const std::filesystem::path& file, const char* sql)
{
    store::Database db = store::Database::Open(file, false);
    db.Exec(sql);
}

// A hosted run file with one conversation (c1) holding one committed turn, one event and the host data; closed again.
std::filesystem::path HostedFile(const char* name)
{
    const std::filesystem::path file = test::FreshTempDir(name) / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    store.AddConversationEvents(id, 0, {nlohmann::json{{"text", "A door opens."}}}, std::nullopt);
    store.CommitHostedTurn(TestHostedTurn(1, id));
    return file;
}

// A version 5 hosted run file as RunStore wrote it, made from SQL and closed again: one ended conversation (c1, with
// TwoParticipants(), folded through 1) holding TestHostedTurn(1, "c1"), and two summaries of npc-1: "First."
// (superseded) and "Condensed." (no conversation).
std::filesystem::path V5HostedFile(const char* name)
{
    const std::filesystem::path file = test::FreshTempDir(name) / "run.db";
    store::Database db = store::Database::Open(file, true);
    db.Exec(V5_SCHEMA_SQL);
    const store::RunMeta meta = HostedMeta();
    const std::vector<std::pair<std::string, std::string>> metaRows = {
        {"schema_version", "5"},
        {"world_id", meta.worldId},
        {"world_hash", meta.worldHash},
        {"model_id", meta.modelId},
        {"rng_seed", std::to_string(meta.rngSeed)},
        {"settings", meta.settings.dump()},
        {"created_at", "2026-11-01T10:00:00Z"},
        {"kind", "hosted"}};
    for (const auto& [key, value] : metaRows) {
        db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
    }
    db.Prepare("INSERT INTO host_data (id, data) VALUES (1, ?1)").Bind(1, TestHostData().dump()).Run();

    // The participants as RunStore stored them: [{"id", "name", "bio"}] in join order.
    nlohmann::json participants = nlohmann::json::array();
    for (const core::Participant& participant : TwoParticipants()) {
        participants.push_back({{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
    }
    db.Prepare("INSERT INTO conversations (id, position, participants, ended, folded_through) "
               "VALUES ('c1', 1, ?1, 1, 1)")
        .Bind(1, participants.dump())
        .Run();

    // The turn's columns as CommitHostedTurn stored them.
    const core::HostedTurn turn = TestHostedTurn(1, "c1");
    const nlohmann::json adjudication = {{"requested", turn.adjudication.requested},
                                         {"action_id", turn.adjudication.actionId},
                                         {"params", turn.adjudication.params},
                                         {"reasons", turn.adjudication.reasons}};
    db.Prepare("INSERT INTO hosted_turns (number, conversation_id, speaker_id, listener_ids, player_text, events, "
               "observation, resolve_output, adjudication, line, timings, events_omitted) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)")
        .Bind(1, turn.number)
        .Bind(2, turn.conversationId)
        .Bind(3, turn.speakerId)
        .Bind(4, nlohmann::json(turn.listenerIds).dump())
        .Bind(5, nlohmann::json(*turn.playerText).dump())
        .Bind(6, nlohmann::json(turn.events).dump())
        .Bind(7, turn.observation.dump())
        .Bind(8, turn.resolveOutput)
        .Bind(9, adjudication.dump())
        .Bind(10, turn.line)
        .Bind(11, turn.timings.dump())
        .Bind(12, turn.eventsOmitted)
        .Run();

    db.Exec("INSERT INTO summaries (from_turn, to_turn, text, actor_id, conversation_id, superseded) "
            "VALUES (1, 1, 'First.', 'npc-1', 'c1', 1);"
            "INSERT INTO summaries (from_turn, to_turn, text, actor_id, conversation_id) "
            "VALUES (1, 1, 'Condensed.', 'npc-1', '');");
    return file;
}

// Every table, index and column of a file, as sorted text: type, name, then each column's position, name, type,
// NOT NULL, default and primary key.
std::vector<std::string> SchemaOf(const std::filesystem::path& file)
{
    store::Database db = store::Database::Open(file, false);
    std::vector<std::string> schema;
    std::vector<std::string> tables;
    {
        store::Statement objects =
            db.Prepare("SELECT type, name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name");
        while (objects.Step()) {
            schema.push_back(objects.ColumnText(0) + " " + objects.ColumnText(1));
            if (objects.ColumnText(0) == "table") {
                tables.push_back(objects.ColumnText(1));
            }
        }
    }
    for (const std::string& table : tables) {
        store::Statement columns = db.Prepare(std::format("PRAGMA table_info({})", table));
        while (columns.Step()) {
            schema.push_back(std::format("{}: {} {} {} {} {} {}", table, columns.ColumnInt(0), columns.ColumnText(1),
                                         columns.ColumnText(2), columns.ColumnInt(3), columns.ColumnText(4),
                                         columns.ColumnInt(5)));
        }
    }
    return schema;
}

// A version 2 run file with only its meta rows, closed again. `extraSql` runs after the schema.
std::filesystem::path V2File(const char* name, const char* extraSql)
{
    const std::filesystem::path file = test::FreshTempDir(name) / "run.db";
    store::Database db = store::Database::Open(file, true);
    db.Exec(V2_SCHEMA_SQL);
    db.Exec(extraSql);
    const std::vector<std::pair<std::string, std::string>> meta = {
        {"schema_version", "2"},   {"world_id", "mini"}, {"world_hash", "0123456789abcdef"},
        {"model_id", "old-model"}, {"rng_seed", "42"},   {"settings", "{}"},
        {"created_at", "2026-11-01T10:00:00Z"}};
    for (const auto& [key, value] : meta) {
        db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
    }
    return file;
}

} // namespace

TEST_CASE("Damaged saved host data and conversations are StoreErrors naming the file and the column",
          "[store][hosted]")
{
    SECTION("host data that is not JSON")
    {
        const std::filesystem::path file = HostedFile("store_damaged_host");
        Damage(file, "UPDATE host_data SET data = 'nope'");
        store::RunStore store = store::RunStore::Open(file);
        CHECK_THROWS_AS(store.HostData(), store::StoreError);
        CHECK_THROWS_WITH(store.HostData(), ContainsSubstring("run.db") && ContainsSubstring("host data"));
    }
    SECTION("host data missing")
    {
        const std::filesystem::path file = HostedFile("store_missing_host");
        Damage(file, "DELETE FROM host_data");
        store::RunStore store = store::RunStore::Open(file);
        CHECK_THROWS_AS(store.HostData(), store::StoreError);
        CHECK_THROWS_WITH(store.HostData(), ContainsSubstring("run.db") && ContainsSubstring("no host data"));
    }
    for (const char* participants : {"nope", "{}", R"([{"id":"a","name":"A"}])", R"([5])"}) {
        SECTION(std::string("participants ") + participants)
        {
            const std::filesystem::path file = HostedFile("store_damaged_participants");
            store::Database db = store::Database::Open(file, false);
            db.Prepare("UPDATE conversation_changes SET participants = ?1").Bind(1, std::string(participants)).Run();
            store::RunStore store = store::RunStore::Open(file);
            CHECK_THROWS_AS(store.Conversations(), store::StoreError);
            CHECK_THROWS_WITH(store.Conversations(), ContainsSubstring("run.db") && ContainsSubstring("'participants'"));
        }
    }
    SECTION("a conversation without a visible change row")
    {
        const std::filesystem::path file = HostedFile("store_missing_change");
        Damage(file, "DELETE FROM conversation_changes");
        store::RunStore store = store::RunStore::Open(file);
        CHECK_THROWS_AS(store.Conversations(), store::StoreError);
        CHECK_THROWS_WITH(store.Conversations(),
                          ContainsSubstring("run.db") && ContainsSubstring("conversation_changes") &&
                              ContainsSubstring("'c1'"));
    }
    SECTION("a missing kind")
    {
        const std::filesystem::path file = HostedFile("store_missing_kind");
        Damage(file, "DELETE FROM run_meta WHERE key = 'kind'");
        CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
        CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("'kind'"));
    }
}

TEST_CASE("Damaged saved hosted turns and events are StoreErrors naming the file and the column", "[store][hosted]")
{
    struct Case {
        const char* sql;
        const char* column;
    };
    const std::vector<Case> turnCases = {
        {"UPDATE hosted_turns SET listener_ids = 'x'", "listener_ids"},
        {"UPDATE hosted_turns SET listener_ids = '{}'", "listener_ids"},
        {"UPDATE hosted_turns SET listener_ids = '[1]'", "listener_ids"},
        {"UPDATE hosted_turns SET player_text = '5'", "player_text"},
        {"UPDATE hosted_turns SET events = 'x'", "events"},
        {"UPDATE hosted_turns SET events = '{}'", "events"},
        {"UPDATE hosted_turns SET events = '[1]'", "events"},
        {R"(UPDATE hosted_turns SET events = '[{"type":"ambient"}]')", "events"},
        {R"(UPDATE hosted_turns SET events = '[{"text":5}]')", "events"},
        {"UPDATE hosted_turns SET observation = 'x'", "observation"},
        {"UPDATE hosted_turns SET observation = '[]'", "observation"},
        {"UPDATE hosted_turns SET adjudication = '{}'", "adjudication"},
        {R"(UPDATE hosted_turns SET adjudication = '{"requested":true,"action_id":"a","params":[],"reasons":[]}')",
         "adjudication"},
        {R"(UPDATE hosted_turns SET adjudication = '{"requested":true,"action_id":"a","params":{},"reasons":[1]}')",
         "adjudication"},
        {"UPDATE hosted_turns SET timings = 'x'", "timings"},
    };
    for (const Case& damaged : turnCases) {
        SECTION(damaged.sql)
        {
            const std::filesystem::path file = HostedFile("store_damaged_turn");
            Damage(file, damaged.sql);
            store::RunStore store = store::RunStore::Open(file);
            CHECK_THROWS_AS(store.HostedTurns("c1"), store::StoreError);
            CHECK_THROWS_WITH(store.HostedTurns("c1"),
                              ContainsSubstring("run.db") && ContainsSubstring(std::string("'") + damaged.column + "'"));
        }
    }

    const std::vector<const char*> eventCases = {"x", "5", "[]", R"({"type":"ambient"})", R"({"text":5})"};
    for (const char* event : eventCases) {
        SECTION(std::string("conversation event ") + event)
        {
            const std::filesystem::path file = HostedFile("store_damaged_event");
            store::Database db = store::Database::Open(file, false);
            db.Prepare("UPDATE conversation_events SET event = ?1").Bind(1, std::string(event)).Run();
            store::RunStore store = store::RunStore::Open(file);
            CHECK_THROWS_AS(store.ConversationEvents("c1"), store::StoreError);
            CHECK_THROWS_WITH(store.ConversationEvents("c1"), ContainsSubstring("run.db") && ContainsSubstring("c1"));
        }
    }
}

TEST_CASE("A stored observation without its actors core, or without the speaker, is a StoreError naming the file, "
          "the turn and the column",
          "[store][hosted]")
{
    struct Case {
        const char* observation;
        const char* column;
    };
    // The second actor of each is the player, Aela; the turn's speaker is npc-1.
    const std::vector<Case> cases = {
        {R"({"note":"fine"})", "observation"},
        {R"({"actors":{}})", "observation"},
        {R"({"actors":[5,{"id":"player","name":"Aela","is_player":true}]})", "observation"},
        {R"({"actors":[{"id":"npc-1","name":"Hulda","is_player":false}]})", "observation"},
        {R"({"actors":[{"id":"npc-1","name":"Hulda","is_player":true},{"id":"player","name":"Aela","is_player":true}]})",
         "observation"},
        {R"({"actors":[{"id":"","name":"Hulda","is_player":false},{"id":"player","name":"Aela","is_player":true}]})",
         "observation"},
        {R"({"actors":[{"id":"npc-1","name":5,"is_player":false},{"id":"player","name":"Aela","is_player":true}]})",
         "observation"},
        {R"({"actors":[{"id":"npc-1","name":"Hulda"},{"id":"player","name":"Aela","is_player":true}]})", "observation"},
        {R"({"actors":[{"id":"npc-1","name":"Hulda","is_player":false,"bio":5},)"
         R"({"id":"player","name":"Aela","is_player":true}]})",
         "observation"},
        {R"({"actors":[{"id":"player","name":"Hulda","is_player":false},{"id":"player","name":"Aela","is_player":true}]})",
         "observation"},
        {R"({"actors":[{"id":"npc-2","name":"Saadia","is_player":false},{"id":"player","name":"Aela","is_player":true}]})",
         "speaker_id"},
    };
    for (const Case& damaged : cases) {
        SECTION(damaged.observation)
        {
            const std::filesystem::path file = HostedFile("store_damaged_actors");
            {
                store::Database db = store::Database::Open(file, false);
                db.Prepare("UPDATE hosted_turns SET observation = ?1").Bind(1, std::string(damaged.observation)).Run();
            }
            store::RunStore store = store::RunStore::Open(file);
            CHECK_THROWS_AS(store.HostedTurns("c1"), store::StoreError);
            CHECK_THROWS_WITH(store.HostedTurns("c1"), ContainsSubstring("run.db") &&
                                                           ContainsSubstring("hosted turn 1") &&
                                                           ContainsSubstring(std::string("'") + damaged.column + "'"));
        }
    }
}

TEST_CASE("A failed upgrade names the file and leaves it a version 2 run", "[store][hosted]")
{
    // A stray table named like a new one makes the upgrade's SQL fail.
    const std::filesystem::path file = V2File("store_upgrade_fails", "CREATE TABLE conversations (x INTEGER);");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("version 2"));

    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "2");
    store::Statement kind = db.Prepare("SELECT COUNT(*) FROM run_meta WHERE key = 'kind'");
    REQUIRE(kind.Step());
    CHECK(kind.ColumnInt(0) == 0);
    store::Statement tables = db.Prepare("SELECT COUNT(*) FROM sqlite_master WHERE name = 'host_data'");
    REQUIRE(tables.Step());
    CHECK(tables.ColumnInt(0) == 0); // The rollback also took back the tables added before the failure.
}

TEST_CASE("New and upgraded files index hosted rows by conversation", "[store][hosted]")
{
    const std::filesystem::path created = HostedFile("store_index_created");
    const std::filesystem::path upgraded = V2File("store_index_upgraded", "");
    store::RunStore::Open(upgraded);
    for (const std::filesystem::path& file : {created, upgraded}) {
        store::Database db = store::Database::Open(file, false);
        store::Statement indexes = db.Prepare("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name IN "
                                              "('hosted_turns_conversation', 'conversation_events_conversation')");
        REQUIRE(indexes.Step());
        CHECK(indexes.ColumnInt(0) == 2);
    }
}

TEST_CASE("FoldConversation moves the folded mark only forward and not past the latest hosted turn",
          "[store][hosted]")
{
    store::RunStore store =
        store::RunStore::CreateHosted(test::FreshTempDir("store_fold_bounds") / "run.db", HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    CHECK_THROWS_AS(store.FoldConversation("c9", ActorSummary("npc-1", "c9", 1, 1, "x")), core::ContractError);
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", id, 1, 3, "Past the latest turn.")),
                    core::ContractError);
    store.FoldConversation(id, ActorSummary("npc-1", id, 1, 2, "Both."));
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", id, 1, 1, "Backwards.")), core::ContractError);
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", id, 1, 2, "Again.")), core::ContractError);
    CHECK(store.HostedSummaries().size() == 1);
    CHECK(store.Conversation(id)->foldedThrough == 2);
}

TEST_CASE("Owned writers refuse a hosted run, and hosted methods refuse an owned run", "[store][hosted]")
{
    const std::filesystem::path dir = test::FreshTempDir("store_kind_guards");
    store::RunStore hosted = store::RunStore::CreateHosted(dir / "hosted.db", HostedMeta(), TestHostData());
    CHECK_THROWS_AS(hosted.CommitTurn(core::TurnRecord{}, nlohmann::json::object()), core::ContractError);
    CHECK_THROWS_AS(hosted.TruncateAfter(0), core::ContractError);

    store::RunMeta ownedMeta = HostedMeta();
    ownedMeta.kind = store::RunKind::Owned;
    store::RunStore owned = store::RunStore::Create(dir / "owned.db", ownedMeta, nlohmann::json::object());
    CHECK_THROWS_AS(owned.ReplaceHostData(TestHostData(), "00000000000000bb"), core::ContractError);
    CHECK_THROWS_AS(owned.HostedTurns("c1"), core::ContractError);
    CHECK_THROWS_AS(owned.ConversationEvents("c1"), core::ContractError);
    CHECK_THROWS_AS(owned.ActorSummaries("npc-1", true), core::ContractError);
    CHECK_THROWS_AS(owned.HostedSummaries(), core::ContractError);
    CHECK_THROWS_AS(owned.AddActorSummary(ActorSummary("npc-1", "c1", 1, 1, "x")), core::ContractError);
    CHECK_THROWS_AS(owned.RememberedThrough("c1", "npc-1"), core::ContractError);
    CHECK_THROWS_AS(owned.PendingShares(), core::ContractError);
}

TEST_CASE("A hosted turn whose text is not UTF-8 still commits", "[store][hosted]")
{
    store::RunStore store =
        store::RunStore::CreateHosted(test::FreshTempDir("store_bad_utf8") / "run.db", HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    const std::string bad = std::string("a") + static_cast<char>(0xFF) + "b";
    core::HostedTurn turn = TestHostedTurn(1, id);
    turn.playerText = bad;
    turn.listenerIds = {bad};
    turn.events = {nlohmann::json{{"text", "ok"}}};
    CHECK_NOTHROW(store.CommitHostedTurn(turn));
    CHECK(store.HostedTurns(id).size() == 1);
}

TEST_CASE("A hosted turn keeps how many events its prompt left out", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_events_omitted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), std::nullopt);
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

TEST_CASE("A version 3 run is upgraded in place to version 6, its turns with no events left out", "[store][hosted]")
{
    // A version 3 file as RunStore wrote it: a version 5 file without version 5's additions and without
    // events_omitted, marked version 3.
    const std::filesystem::path file = HostedFile("store_upgrade_v3");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "ALTER TABLE hosted_turns DROP COLUMN events_omitted;"
                 "UPDATE run_meta SET value = '3' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        const std::vector<core::HostedTurn> turns = store.HostedTurns("c1");
        REQUIRE(turns.size() == 1);
        CHECK(turns[0].eventsOmitted == 0);
        CHECK(store.ConversationEvents("c1").size() == 1);
        CHECK(store.HostedSummaries().empty());
        CHECK(store.PendingShares().empty());
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "6");
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

TEST_CASE("A hosted run keeps summaries per actor, with their conversation, teller and supersede mark",
          "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_actor_summaries") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    store.CommitHostedTurn(TestHostedTurn(1, id));
    store.CommitHostedTurn(TestHostedTurn(2, id));

    // A fold by the speaker moves the conversation's mark and the speaker's own.
    const std::int64_t first = store.FoldConversation(id, ActorSummary("npc-1", id, 1, 1, "Hulda greeted Aela."));
    CHECK(first > 0);
    CHECK(store.Conversation(id)->foldedThrough == 1);
    CHECK(store.RememberedThrough(id, "npc-1") == 1);
    CHECK(store.RememberedThrough(id, "npc-2") == 0); // No row: 0.

    // A catch-up fold moves only the actor's mark.
    store.RememberFold(ActorSummary("npc-2", id, 1, 1, "Saadia saw Hulda greet Aela."));
    CHECK(store.RememberedThrough(id, "npc-2") == 1);
    CHECK(store.Conversation(id)->foldedThrough == 1);

    // A shared copy.
    core::Summary copy = ActorSummary("npc-2", id, 1, 1, "Hulda greeted Aela.");
    copy.toldBy = "npc-1";
    copy.sourceId = first;
    const std::int64_t copied = store.AddActorSummary(copy);

    const std::vector<core::Summary> saadia = store.ActorSummaries("npc-2", false);
    REQUIRE(saadia.size() == 2);
    CHECK(saadia[0].text == "Saadia saw Hulda greet Aela.");
    CHECK(saadia[1].id == copied);
    CHECK(saadia[1].toldBy == "npc-1");
    CHECK(saadia[1].sourceId == first);
    CHECK(saadia[1].conversationId == id);

    // Condensing supersedes the shown ones and adds one with no conversation and no teller.
    core::Summary condensed = ActorSummary("npc-2", "", 1, 1, "Saadia remembers the greeting.");
    const std::int64_t condensedId = store.CondenseActorSummaries("npc-2", condensed);
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-2", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == condensedId);
    CHECK(shown[0].conversationId.empty());
    const std::vector<core::Summary> all = store.ActorSummaries("npc-2", true);
    REQUIRE(all.size() == 3);
    CHECK(all[0].superseded);
    CHECK(all[1].superseded);
    CHECK_FALSE(all[2].superseded);
    CHECK(store.HostedSummaries().size() == 4); // Hulda's one and Saadia's three.

    // The marks only move forward.
    store.SetRememberedThrough(id, "npc-2", 2);
    CHECK(store.RememberedThrough(id, "npc-2") == 2);
    CHECK_THROWS_AS(store.SetRememberedThrough(id, "npc-2", 1), core::ContractError);
    store.MarkFolded(id, 2);
    CHECK(store.Conversation(id)->foldedThrough == 2);
}

TEST_CASE("A hosted run keeps pending shares until they are applied", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_pending_shares") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    const std::int64_t share = store.AddPendingShare({0, "npc-1", "npc-2", id});
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == share);
    CHECK(pending[0].fromActor == "npc-1");
    CHECK(pending[0].toActor == "npc-2");
    CHECK(pending[0].conversationId == id);
    store.MarkShareApplied(share, std::nullopt);
    CHECK(store.PendingShares().empty());
}

TEST_CASE("Run-wide summary methods are the owned game's; per-actor folds need an actor", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_summary_guards") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
    store.CommitHostedTurn(TestHostedTurn(1, id));
    CHECK_THROWS_AS(store.Summaries(), core::ContractError);
    CHECK_THROWS_AS(store.AddSummary({1, 1, "x"}), core::ContractError);
    CHECK_THROWS_AS(store.ReplaceSummaries({1, 1, "x"}), core::ContractError);
    CHECK_THROWS_AS(store.FoldConversation(id, {1, 1, "x"}), core::ContractError); // No actor.
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", "c9", 1, 1, "x")), core::ContractError);
}

TEST_CASE("A version 4 hosted run is upgraded through version 5: each summary goes to the NPCs who witnessed it",
          "[store][hosted][memory]")
{
    // A version 4 file as RunStore wrote it: a version 5 file without the new columns and tables, marked version 4,
    // with one run-wide summary of turn 1 (Hulda, npc-1, spoke to Aela, the player).
    const std::filesystem::path file = HostedFile("store_upgrade_v4");
    Damage(file, "UPDATE conversations SET folded_through = 1;"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'Hulda greeted Aela.');");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        // Hulda, the NPC speaker, has it; Aela, the player, has none.
        const std::vector<core::Summary> hulda = store.ActorSummaries("npc-1", true);
        REQUIRE(hulda.size() == 1);
        CHECK(hulda[0].text == "Hulda greeted Aela.");
        CHECK(hulda[0].conversationId == "c1");
        CHECK(hulda[0].fromTurn == 1);
        CHECK(hulda[0].toTurn == 1);
        CHECK(store.ActorSummaries("player", true).empty());
        CHECK(store.RememberedThrough("c1", "npc-1") == 1);
        CHECK(store.RememberedThrough("c1", "player") == 0);
        CHECK(store.HostedSummaries().size() == 1); // The run-wide row is gone.
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "6");
}

TEST_CASE("A version 4 owned run is upgraded to version 6 with its summaries unchanged", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_upgrade_v4_owned") / "run.db";
    store::RunMeta owned = HostedMeta();
    owned.kind = store::RunKind::Owned;
    owned.worldId = "mini";
    {
        store::RunStore store = store::RunStore::Create(file, owned, nlohmann::json::object());
        store.AddSummary({1, 2, "A climb."});
        store.AddSummary({3, 4, "A fall."});
    }
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Owned);
        const std::vector<core::Summary> summaries = store.Summaries();
        REQUIRE(summaries.size() == 2);
        CHECK(summaries[0].fromTurn == 1);
        CHECK(summaries[0].toTurn == 2);
        CHECK(summaries[0].text == "A climb.");
        CHECK(summaries[0].actorId.empty());
        CHECK(summaries[1].text == "A fall.");
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "6");
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

TEST_CASE("A version 4 interleaved fold goes only to its conversation's witnesses; a condense over two conversations "
          "goes to every witness with no conversation",
          "[store][hosted][memory]")
{
    // c1 (Hulda and Aela) has turns 1 and 3; c2 (Saadia and Aela) has turns 2 and 4. D1 stored a fold of c1's turns 1
    // and 3 as the range 1..3, around c2's turn 2: its end turns are both c1's, so it is c1's fold. A condense over
    // both conversations ends on turns of two conversations (1..4). A fold of c1's turn 1 alone is c1's too.
    const std::filesystem::path file = test::FreshTempDir("store_upgrade_v4_two") / "run.db";
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        const std::string c1 = store.CreateConversation(TwoParticipants(), std::nullopt);
        const std::string c2 = store.CreateConversation({{"npc-2", "Saadia", ""}, {"player", "Aela", ""}}, std::nullopt);
        auto saadia = [&c2](int number) {
            core::HostedTurn turn = TestHostedTurn(number, c2);
            turn.speakerId = "npc-2";
            turn.observation = nlohmann::json::parse(R"({"actors": [{"id": "npc-2", "name": "Saadia", "is_player": false},
                                                                    {"id": "player", "name": "Aela", "is_player": true}]})");
            return turn;
        };
        store.CommitHostedTurn(TestHostedTurn(1, c1));
        store.CommitHostedTurn(saadia(2));
        store.CommitHostedTurn(TestHostedTurn(3, c1));
        store.CommitHostedTurn(saadia(4));
    }
    Damage(file, "UPDATE conversations SET folded_through = 3 WHERE id = 'c1';"
                 "UPDATE conversations SET folded_through = 4 WHERE id = 'c2';"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'Hulda greeted Aela.');"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 3, 'Hulda talked with Aela twice.');"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 4, 'Both talks.');");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");

    store::RunStore store = store::RunStore::Open(file);
    // Hulda has both of c1's folds, labelled c1, and the condense, with no conversation.
    const std::vector<core::Summary> hulda = store.ActorSummaries("npc-1", true);
    REQUIRE(hulda.size() == 3);
    CHECK(hulda[0].text == "Hulda greeted Aela.");
    CHECK(hulda[0].conversationId == "c1");
    CHECK(hulda[1].text == "Hulda talked with Aela twice.");
    CHECK(hulda[1].conversationId == "c1");
    CHECK(hulda[1].fromTurn == 1);
    CHECK(hulda[1].toTurn == 3);
    CHECK(hulda[2].text == "Both talks.");
    CHECK(hulda[2].conversationId.empty());
    // Saadia witnessed turn 2, inside the interleaved fold's range, but not c1's turns: she has only the condense.
    const std::vector<core::Summary> saadia = store.ActorSummaries("npc-2", true);
    REQUIRE(saadia.size() == 1);
    CHECK(saadia[0].text == "Both talks.");
    CHECK(saadia[0].conversationId.empty());
    CHECK(store.ActorSummaries("player", true).empty());
    CHECK(store.RememberedThrough("c1", "npc-1") == 3);
    CHECK(store.RememberedThrough("c2", "npc-2") == 4);
}

TEST_CASE("A version 4 summary whose range holds no turn fails the upgrade, naming the file and the summary",
          "[store][hosted][memory]")
{
    // Turn 1 is the run's only turn: a summary of turns 5-6 covers none.
    const std::filesystem::path file = HostedFile("store_upgrade_v4_no_turn");
    Damage(file, "INSERT INTO summaries (from_turn, to_turn, text) VALUES (5, 6, 'Nothing happened.');");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("summary 1") &&
                                                       ContainsSubstring("version 4"));

    // The upgrade rolled back: still version 4, its run-wide summary kept.
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "4");
    store::Statement summaries = db.Prepare("SELECT COUNT(*) FROM summaries");
    REQUIRE(summaries.Step());
    CHECK(summaries.ColumnInt(0) == 1);
}

TEST_CASE("A version 4 summary whose end turn is not a hosted turn fails the upgrade, naming the file and the summary",
          "[store][hosted][memory]")
{
    // Turn 1 is the run's only turn: a summary of turns 1-2 starts on it but ends on no turn, so its conversation
    // cannot be told.
    const std::filesystem::path file = HostedFile("store_upgrade_v4_no_end");
    Damage(file, "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 2, 'Half a talk.');");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("summary 1") &&
                                                       ContainsSubstring("1..2"));
}

TEST_CASE("A version 4 hosted turn whose listener id is not a string fails the upgrade, naming the file and the column",
          "[store][hosted][memory]")
{
    const std::filesystem::path file = HostedFile("store_upgrade_v4_bad_listener");
    Damage(file, "UPDATE hosted_turns SET listener_ids = '[\"player\", 5]' WHERE number = 1;");
    Damage(file, DROP_VERSION_6_SQL);
    Damage(file, DROP_VERSION_5_SQL);
    Damage(file, "UPDATE run_meta SET value = '4' WHERE key = 'schema_version';");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("listener_ids") &&
                                                       ContainsSubstring("hosted turn 1"));
}

TEST_CASE("A hosted run with a memory mark of a conversation it does not have is a StoreError naming the file and the "
          "column",
          "[store][hosted][memory]")
{
    const std::filesystem::path file = HostedFile("store_mark_unknown_conversation");
    Damage(file, "INSERT INTO actor_memory (conversation_id, actor_id, remembered_through) VALUES ('c9', 'npc-1', 1);");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") &&
                                                       ContainsSubstring("actor_memory.conversation_id") &&
                                                       ContainsSubstring("'c9'"));
}

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

TEST_CASE("A share applied in a run with no clock is not pending afterwards, and its row stays",
          "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_applied_share_no_clock") / "run.db";
    std::int64_t shareId = 0;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        const std::string id = store.CreateConversation(TwoParticipants(), std::nullopt);
        shareId = store.AddPendingShare({0, "npc-1", "npc-2", id});
        REQUIRE(store.PendingShares().size() == 1);
        // Applied with no clock: applied all the same, and the run still has no clock.
        store.MarkShareApplied(shareId, std::nullopt);
        CHECK(store.PendingShares().empty());
        CHECK_FALSE(store.LatestClock().has_value());
    }
    // It stays applied when the run is opened again, and the row is kept.
    CHECK(store::RunStore::Open(file).PendingShares().empty());
    store::Database db = store::Database::Open(file, false);
    store::Statement row = db.Prepare("SELECT applied, applied_clock FROM pending_shares WHERE id = ?1");
    row.Bind(1, shareId);
    REQUIRE(row.Step());
    CHECK(row.ColumnInt(0) == 1);
    CHECK(row.ColumnIsNull(1));
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

TEST_CASE("A version 5 superseded summary with no later condense fails the upgrade, naming the file and the summary",
          "[store][hosted][rollback]")
{
    const std::filesystem::path file = V5HostedFile("store_upgrade_v5_no_condense");
    Damage(file, "DELETE FROM summaries WHERE text = 'Condensed.';");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file),
                      ContainsSubstring("run.db") && ContainsSubstring("summary 1") && ContainsSubstring("version 5"));
    // The upgrade rolled back: still version 5.
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "5");
}

TEST_CASE("A new file and an upgraded one have the same tables, indexes and columns", "[store][hosted][rollback]")
{
    // Hosted: a new file and a version 5 one upgraded.
    const std::filesystem::path created = HostedFile("store_schema_created");
    const std::filesystem::path upgraded = V5HostedFile("store_schema_upgraded");
    store::RunStore::Open(upgraded);
    CHECK(SchemaOf(created) == SchemaOf(upgraded));

    // Owned: a new file and a version 2 one upgraded.
    store::RunMeta owned = HostedMeta();
    owned.kind = store::RunKind::Owned;
    const std::filesystem::path createdOwned = test::FreshTempDir("store_schema_created_owned") / "run.db";
    store::RunStore::Create(createdOwned, owned, nlohmann::json::object());
    const std::filesystem::path upgradedOwned = V2File("store_schema_upgraded_owned", "");
    store::RunStore::Open(upgradedOwned);
    CHECK(SchemaOf(createdOwned) == SchemaOf(upgradedOwned));
}

TEST_CASE("A summary keeps every condense that superseded it: undoing a later condense leaves the earlier one in force",
          "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_supersessions") / "run.db";
    std::string id;
    {
        // S (turn 1) and S2 (turn 2) are condensed into C1.
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), std::nullopt);
        store.CommitHostedTurn(TestHostedTurn(1, id));
        store.CommitHostedTurn(TestHostedTurn(2, id));
        store.FoldConversation(id, ActorSummary("npc-1", id, 1, 1, "S."));
        store.FoldConversation(id, ActorSummary("npc-1", id, 2, 2, "S2."));
        store.CondenseActorSummaries("npc-1", ActorSummary("npc-1", "", 1, 2, "C1."));
    }
    // An open rollback hides C1 and S2: S is shown again.
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 0.5, 'open', '{}');"
                 "UPDATE summaries SET hidden_by = 1 WHERE text IN ('C1.', 'S2.');");
    {
        store::RunStore store = store::RunStore::Open(file);
        const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].text == "S.");
        CHECK(shown[0].supersededBy == 0);
        // S is condensed into C2 while the rollback is open: C2 is made in it.
        const std::int64_t c2 = store.CondenseActorSummaries("npc-1", ActorSummary("npc-1", "", 1, 1, "C2."));
        const std::vector<core::Summary> now = store.ActorSummaries("npc-1", true);
        REQUIRE(now.size() == 2);
        CHECK(now[0].text == "S.");
        CHECK(now[0].supersededBy == c2);
        CHECK(now[1].id == c2);
        CHECK(now[1].madeIn == 1);
    }
    // The undo, by hand: C2 and its links go, C1 and S2 are shown again.
    Damage(file, "DELETE FROM summary_supersessions "
                 "WHERE condensed_id = (SELECT id FROM summaries WHERE text = 'C2.');"
                 "DELETE FROM summaries WHERE text = 'C2.';"
                 "UPDATE summaries SET hidden_by = 0 WHERE hidden_by = 1;"
                 "UPDATE rollbacks SET status = 'undone' WHERE id = 1;");
    store::RunStore store = store::RunStore::Open(file);
    const std::vector<core::Summary> all = store.ActorSummaries("npc-1", true);
    REQUIRE(all.size() == 3);
    const auto c1 =
        std::find_if(all.begin(), all.end(), [](const core::Summary& summary) { return summary.text == "C1."; });
    REQUIRE(c1 != all.end());
    for (const core::Summary& summary : all) {
        if (summary.text != "C1.") {
            CHECK(summary.superseded);
            CHECK(summary.supersededBy == c1->id);
        }
    }
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].text == "C1.");
}

TEST_CASE("Every hosted reader skips hidden rows", "[store][hosted][rollback]")
{
    // A run with one conversation started at 1.0, changed at 2.0 (one participant left) and ended at 3.0, with an event
    // at 1.0 and one at 2.0, and a turn at 1.5.
    const std::filesystem::path file = test::FreshTempDir("store_hidden_rows") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), 1.0);
        store.AddConversationEvents(id, 0, {nlohmann::json{{"text", "Early."}}}, 1.0);
        core::HostedTurn turn = TestHostedTurn(1, id);
        turn.clock = 1.5;
        store.CommitHostedTurn(turn);
        store.AddConversationEvents(id, 1, {nlohmann::json{{"text", "Late."}}}, 2.0);
        store.SetParticipants(id, {TwoParticipants().at(0)}, 2.0);
        store.EndConversation(id, 3.0);
    }
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 1.75, 'open', '{}');");

    SECTION("a hidden conversation is not there, and writers refuse it")
    {
        Damage(file, "UPDATE conversations SET hidden_by = 1;");
        store::RunStore store = store::RunStore::Open(file);
        CHECK_FALSE(store.Conversation(id).has_value());
        CHECK(store.Conversations().empty());
        CHECK_THROWS_AS(store.CommitHostedTurn(TestHostedTurn(2, id)), core::ContractError);
        CHECK_THROWS_AS(store.EndConversation(id, std::nullopt), core::ContractError);
    }
    SECTION("hidden change rows fall back to the latest visible one")
    {
        Damage(file, "UPDATE conversation_changes SET hidden_by = 1 WHERE clock > 1.75;");
        store::RunStore store = store::RunStore::Open(file);
        const std::optional<core::Conversation> conversation = store.Conversation(id);
        REQUIRE(conversation);
        CHECK(conversation->participants.size() == 2);
        CHECK_FALSE(conversation->ended);
    }
    SECTION("hidden events are not read")
    {
        Damage(file, "UPDATE conversation_events SET hidden_by = 1 WHERE clock > 1.75;");
        store::RunStore store = store::RunStore::Open(file);
        const std::vector<core::ConversationEvent> events = store.ConversationEvents(id);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.at("text") == "Early.");
    }
}

TEST_CASE("Hidden shares are not pending; a share a rollback unapplied is pending again", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_hidden_shares") / "run.db";
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        const std::string id = store.CreateConversation(TwoParticipants(), 1.0);
        store.AddPendingShare({0, "npc-1", "npc-2", id, 1.0});
        const std::int64_t applied = store.AddPendingShare({0, "npc-1", "npc-3", id, 1.0});
        store.MarkShareApplied(applied, 2.0);
        REQUIRE(store.PendingShares().size() == 1);
    }
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 1.5, 'open', '{}');"
                 "UPDATE pending_shares SET hidden_by = 1 WHERE to_actor = 'npc-2';"
                 "UPDATE pending_shares SET unapplied_by = 1 WHERE to_actor = 'npc-3';");
    store::RunStore store = store::RunStore::Open(file);
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].toActor == "npc-3");
    CHECK(pending[0].appliedClock == 2.0);
    CHECK(pending[0].unappliedBy == 1);
}

TEST_CASE("The latest clock skips hidden rows and the applied clocks of unapplied shares", "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_latest_clock_hidden") / "run.db";
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        const std::string id = store.CreateConversation(TwoParticipants(), 1.0);
        core::HostedTurn turn = TestHostedTurn(1, id);
        turn.clock = 2.0;
        store.CommitHostedTurn(turn);
        const std::int64_t share = store.AddPendingShare({0, "npc-1", "npc-2", id, 2.5});
        store.MarkShareApplied(share, 4.0);
        REQUIRE(store.LatestClock() == 4.0);
    }
    // Unapplied: its applied clock no longer counts, its creation still does.
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (1, 1.5, 'open', '{}');"
                 "UPDATE pending_shares SET unapplied_by = 1;");
    CHECK(store::RunStore::Open(file).LatestClock() == 2.5);
    // Hidden: neither the share nor the turn counts; the conversation's start does.
    Damage(file, "UPDATE pending_shares SET hidden_by = 1; UPDATE hosted_turns SET hidden_by = 1;");
    CHECK(store::RunStore::Open(file).LatestClock() == 1.0);
}

TEST_CASE("A summary written while a rollback is open is made in it; two open rollbacks are damaged saved data",
          "[store][hosted][rollback]")
{
    const std::filesystem::path file = test::FreshTempDir("store_made_in") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants(), std::nullopt);
        store.CommitHostedTurn(TestHostedTurn(1, id));
        store.AddActorSummary(ActorSummary("npc-1", id, 1, 1, "Before."));
    }
    // An undone rollback is not open; rollback 3 is.
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (2, 0.5, 'undone', '{}');"
                 "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (3, 0.5, 'open', '{}');");
    {
        store::RunStore store = store::RunStore::Open(file);
        store.AddActorSummary(ActorSummary("npc-1", id, 1, 1, "During."));
        const std::vector<core::Summary> summaries = store.ActorSummaries("npc-1", true);
        REQUIRE(summaries.size() == 2);
        CHECK(summaries[0].madeIn == 0);
        CHECK(summaries[1].madeIn == 3);
    }
    // A second open rollback: the run no longer opens, so nothing can be written in either.
    Damage(file, "INSERT INTO rollbacks (id, game_time, status, saved_marks) VALUES (4, 0.5, 'open', '{}');");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("rollbacks"));
}
