#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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

    store.FoldConversation(id, ActorSummary("npc-1", id, 1, 2, "They met."));
    CHECK(store.Conversation(id)->foldedThrough == 2);
    const std::vector<core::Summary> summaries = store.ActorSummaries("npc-1", true);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "They met.");
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", id, 3, 2, "Backwards.")), core::ContractError);
    CHECK(store.HostedSummaries().size() == 1);

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

TEST_CASE("A version 2 run is upgraded in place to an owned version 5 run, its data unchanged", "[store][hosted]")
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

    // The file now says version 5 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "5");
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

namespace {

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
    const std::string id = store.CreateConversation(TwoParticipants());
    store.AddConversationEvents(id, 0, {nlohmann::json{{"text", "A door opens."}}});
    store.CommitHostedTurn(TestHostedTurn(1, id));
    return file;
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
            db.Prepare("UPDATE conversations SET participants = ?1").Bind(1, std::string(participants)).Run();
            store::RunStore store = store::RunStore::Open(file);
            CHECK_THROWS_AS(store.Conversations(), store::StoreError);
            CHECK_THROWS_WITH(store.Conversations(), ContainsSubstring("run.db") && ContainsSubstring("'participants'"));
        }
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
    const std::string id = store.CreateConversation(TwoParticipants());
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
    const std::string id = store.CreateConversation(TwoParticipants());
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

TEST_CASE("A version 3 run is upgraded in place to version 5, its turns with no events left out", "[store][hosted]")
{
    // A version 3 file as RunStore wrote it: a version 5 file without version 5's additions and without
    // events_omitted, marked version 3.
    const std::filesystem::path file = HostedFile("store_upgrade_v3");
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
    CHECK(version.ColumnText(0) == "5");
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
    const std::string id = store.CreateConversation(TwoParticipants());
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

TEST_CASE("A hosted run keeps pending shares until they are removed", "[store][hosted][memory]")
{
    const std::filesystem::path file = test::FreshTempDir("store_pending_shares") / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
    const std::string id = store.CreateConversation(TwoParticipants());
    const std::int64_t share = store.AddPendingShare({0, "npc-1", "npc-2", id});
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == share);
    CHECK(pending[0].fromActor == "npc-1");
    CHECK(pending[0].toActor == "npc-2");
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
    CHECK_THROWS_AS(store.ReplaceSummaries({1, 1, "x"}), core::ContractError);
    CHECK_THROWS_AS(store.FoldConversation(id, {1, 1, "x"}), core::ContractError); // No actor.
    CHECK_THROWS_AS(store.FoldConversation(id, ActorSummary("npc-1", "c9", 1, 1, "x")), core::ContractError);
}

TEST_CASE("A version 4 hosted run is upgraded to version 5: each summary goes to the NPCs who witnessed it",
          "[store][hosted][memory]")
{
    // A version 4 file as RunStore wrote it: a version 5 file without the new columns and tables, marked version 4,
    // with one run-wide summary of turn 1 (Hulda, npc-1, spoke to Aela, the player).
    const std::filesystem::path file = HostedFile("store_upgrade_v4");
    Damage(file, "UPDATE conversations SET folded_through = 1;"
                 "INSERT INTO summaries (from_turn, to_turn, text) VALUES (1, 1, 'Hulda greeted Aela.');");
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
    CHECK(version.ColumnText(0) == "5");
}

TEST_CASE("A version 4 owned run is upgraded to version 5 with its summaries unchanged", "[store][hosted][memory]")
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
