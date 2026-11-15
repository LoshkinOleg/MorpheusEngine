#include "store/RunStore.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/Text.h"

namespace morpheus::store {

namespace {

constexpr const char* SCHEMA_SQL = R"sql(
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
)sql";

// The tables version 3 adds, with version 4's events_omitted: a hosted run's host data, its conversations, its turns
// and the events recorded between them. Created in every file, so a version 2 file is upgraded by adding them.
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
)sql";

// What version 4 adds to a version 3 file: each hosted turn's count of the events its prompt left out, 0 for the
// turns already played.
constexpr const char* EVENTS_OMITTED_SQL =
    "ALTER TABLE hosted_turns ADD COLUMN events_omitted INTEGER NOT NULL DEFAULT 0;";

// What version 5 adds to a version 4 file (per-speaker memory spec, section 5), in two parts. The summaries' per-actor
// columns, which a new file has from SCHEMA_SQL: for upgrades only. A hosted run's run-wide summaries are then moved to
// their NPC witnesses (RunStore::Open).
constexpr const char* PER_ACTOR_COLUMNS_SQL = R"sql(
ALTER TABLE summaries ADD COLUMN actor_id TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN conversation_id TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN told_by TEXT NOT NULL DEFAULT '';
ALTER TABLE summaries ADD COLUMN source_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE summaries ADD COLUMN superseded INTEGER NOT NULL DEFAULT 0;
)sql";

// The per-speaker memory tables and the summaries' actor index: for new files and upgrades. actor_memory holds how far
// each NPC remembers each conversation; pending_shares the shares waiting for the teller's memory to be complete.
constexpr const char* PER_ACTOR_TABLES_SQL = R"sql(
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

// What version 6 adds (rollback spec, section 4), in three parts, for new files and upgrades alike: a new file is made
// with the version 5 texts above and then takes these, as an upgraded one does, so both end with the same tables and
// columns. The columns: a clock (null allowed: a row without one is never hidden) and a hidden mark (the rollback that
// hid it) on the hosted rows, the rollback a summary was written in, and a share's application: whether it is applied
// (a flag, since a run with no clock applies shares with no clock) and when.
constexpr const char* V6_COLUMNS_SQL = R"sql(
ALTER TABLE hosted_turns ADD COLUMN clock REAL;
ALTER TABLE hosted_turns ADD COLUMN hidden_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE conversation_events ADD COLUMN clock REAL;
ALTER TABLE conversation_events ADD COLUMN hidden_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE conversations ADD COLUMN clock REAL;
ALTER TABLE conversations ADD COLUMN hidden_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE summaries ADD COLUMN clock REAL;
ALTER TABLE summaries ADD COLUMN hidden_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE summaries ADD COLUMN made_in INTEGER NOT NULL DEFAULT 0;
ALTER TABLE pending_shares ADD COLUMN clock REAL;
ALTER TABLE pending_shares ADD COLUMN applied_clock REAL;
ALTER TABLE pending_shares ADD COLUMN unapplied_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE pending_shares ADD COLUMN hidden_by INTEGER NOT NULL DEFAULT 0;
ALTER TABLE pending_shares ADD COLUMN applied INTEGER NOT NULL DEFAULT 0;
)sql";

// The tables: a conversation's history of participants and ends, the rollbacks, and which condensed summaries
// superseded which summaries. A summary can be condensed more than once (a condense a rollback hid leaves it shown,
// and a later condense takes it again), so every link is kept; it is superseded while one of them names a visible
// summary.
constexpr const char* V6_TABLES_SQL = R"sql(
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
CREATE TABLE summary_supersessions (
    summary_id   INTEGER NOT NULL,
    condensed_id INTEGER NOT NULL,
    PRIMARY KEY (summary_id, condensed_id)
);
)sql";

// The columns version 6 replaces: superseded by summary_supersessions, a conversation's participants and end by its
// change rows. An upgrade moves their data first.
constexpr const char* V6_DROPPED_COLUMNS_SQL = R"sql(
ALTER TABLE summaries DROP COLUMN superseded;
ALTER TABLE conversations DROP COLUMN participants;
ALTER TABLE conversations DROP COLUMN ended;
)sql";

// The columns ReadConversation reads from a conversation's start row, in its order.
constexpr const char* CONVERSATION_COLUMNS = "id, folded_through, clock";

// The columns ReadSummaryRow reads, in its order, from `summaries s`. The supersession is computed from
// summary_supersessions: superseded_by is the smallest id among the visible condenses that superseded the summary (0
// for none), and superseded is true when there is one.
constexpr const char* SUMMARY_COLUMNS =
    "s.id, s.from_turn, s.to_turn, s.text, s.actor_id, s.conversation_id, s.told_by, s.source_id, "
    "COALESCE((SELECT MIN(c.id) FROM summary_supersessions l JOIN summaries c ON c.id = l.condensed_id "
    "WHERE l.summary_id = s.id AND c.hidden_by = 0), 0) AS superseded_by, "
    "s.made_in, s.clock, "
    "EXISTS (SELECT 1 FROM summary_supersessions l JOIN summaries c ON c.id = l.condensed_id "
    "WHERE l.summary_id = s.id AND c.hidden_by = 0) AS superseded";

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

// Whether a stored event is what the host sent: a JSON object with a string `text`.
bool EventIsWhole(const nlohmann::json& event)
{
    return event.is_object() && event.contains("text") && event.at("text").is_string();
}

// JSON as stored text. Text that is not UTF-8 (a player's, a reason quoting model output) becomes U+FFFD instead of
// throwing at commit.
std::string DumpText(const nlohmann::json& json)
{
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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

// Stored spelling of an embedding kind. TruncateAfter's SQL relies on "turn" and "note".
const char* KindName(EmbeddingKind kind)
{
    switch (kind) {
    case EmbeddingKind::Lore:
        return "lore";
    case EmbeddingKind::Turn:
        return "turn";
    case EmbeddingKind::Note:
        return "note";
    case EmbeddingKind::Summary:
        return "summary";
    }
    throw morpheus::core::ContractError("KindName: unhandled EmbeddingKind");
}

} // namespace

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
            // The version 5 schema, then version 6's changes as an upgrade makes them (the file has no rows to move).
            db.Exec(SCHEMA_SQL);
            db.Exec(HOSTED_SCHEMA_SQL);
            db.Exec(PER_ACTOR_TABLES_SQL);
            db.Exec(V6_COLUMNS_SQL);
            db.Exec(V6_TABLES_SQL);
            db.Exec(V6_DROPPED_COLUMNS_SQL);

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

RunStore RunStore::Open(const std::filesystem::path& file)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    if (!exists) {
        throw StoreError(std::format("run database '{}' does not exist", core::PathText(file)));
    }
    Database db = Database::Open(file, false);

    // Read the key/value meta table in one pass.
    std::map<std::string, std::string> values;
    {
        Statement query = db.Prepare("SELECT key, value FROM run_meta");
        while (query.Step()) {
            values.emplace(query.ColumnText(0), query.ColumnText(1));
        }
    }
    auto value = [&](const char* key) -> const std::string& {
        const auto it = values.find(key);
        if (it == values.end()) {
            throw StoreError(std::format("'{}': run_meta has no '{}'", core::PathText(file), key));
        }
        return it->second;
    };

    // Version 5's move of a version 3 or 4 hosted run's summaries, inside the upgrade's transaction. Each run-wide
    // summary's conversation is decided by its two end turns: a D1 fold started and ended on turns of the conversation
    // it folded (its range can hold another conversation's turns played in between), so one conversation at both ends
    // is that conversation's fold, which goes to the NPC witnesses of that conversation's turns in the range, labelled
    // with it. Two conversations at the ends is a condense over both: it goes to the NPC witnesses of every turn in the
    // range, with no conversation. The run-wide row goes. Each NPC witness's mark in a conversation starts at the
    // conversation's folded_through.
    auto moveSummaries = [&db, &file] {
        struct Witnessed {
            std::string conversationId = {};
            std::vector<std::string> npcs = {};
        };
        // Every turn's conversation and NPC witnesses (the speaker and the listeners who are not the player).
        std::map<int, Witnessed> turns;
        {
            Statement turnQuery = db.Prepare("SELECT number, conversation_id, speaker_id, listener_ids, observation "
                                             "FROM hosted_turns ORDER BY number");
            while (turnQuery.Step()) {
                const int number = static_cast<int>(turnQuery.ColumnInt(0));
                const nlohmann::json listeners = nlohmann::json::parse(turnQuery.ColumnText(3), nullptr, false);
                const nlohmann::json observation = nlohmann::json::parse(turnQuery.ColumnText(4), nullptr, false);
                const core::ActorsCore actorsCore = core::ReadActors(observation);
                // Every listener is an actor id, as every normal read of a turn requires.
                const bool listenersRead =
                    !listeners.is_discarded() && listeners.is_array() &&
                    std::all_of(listeners.begin(), listeners.end(),
                                [](const nlohmann::json& listener) { return listener.is_string(); });
                if (!listenersRead || !actorsCore.faults.empty()) {
                    throw StoreError(std::format("'{}': hosted turn {} cannot be upgraded: its listener_ids or "
                                                 "observation is damaged",
                                                 core::PathText(file), number));
                }
                auto isPlayer = [&actorsCore](const std::string& id) {
                    return std::any_of(actorsCore.actors.begin(), actorsCore.actors.end(),
                                       [&id](const core::Actor& actor) { return actor.id == id && actor.isPlayer; });
                };
                Witnessed witnessed;
                witnessed.conversationId = turnQuery.ColumnText(1);
                witnessed.npcs.push_back(turnQuery.ColumnText(2));
                for (const nlohmann::json& listener : listeners) {
                    if (!isPlayer(listener.get<std::string>())) {
                        witnessed.npcs.push_back(listener.get<std::string>());
                    }
                }
                turns.emplace(number, std::move(witnessed));
            }
        }

        // The run-wide summaries, read whole before any insert into the same table.
        struct RunWide {
            std::int64_t id = 0;
            int fromTurn = 0;
            int toTurn = 0;
            std::string text = {};
        };
        std::vector<RunWide> runWide;
        {
            Statement summaryQuery = db.Prepare("SELECT id, from_turn, to_turn, text FROM summaries ORDER BY id");
            while (summaryQuery.Step()) {
                runWide.push_back({summaryQuery.ColumnInt(0), static_cast<int>(summaryQuery.ColumnInt(1)),
                                   static_cast<int>(summaryQuery.ColumnInt(2)), summaryQuery.ColumnText(3)});
            }
        }

        // Each one copied to the NPC witnesses it belongs to, then removed.
        for (const RunWide& summary : runWide) {
            // Every fold and condense starts and ends on turns that exist: a summary whose end turns are not hosted
            // turns (or come in the wrong order) is damaged data, and the upgrade rolls back.
            const auto first = turns.find(summary.fromTurn);
            const auto last = turns.find(summary.toTurn);
            if (first == turns.end() || last == turns.end() || summary.fromTurn > summary.toTurn) {
                throw StoreError(std::format("'{}': summary {} covers turns {}..{}, which do not start and end on "
                                             "hosted turns",
                                             core::PathText(file), summary.id, summary.fromTurn, summary.toTurn));
            }
            // One conversation at both ends: that conversation's fold, labelled with it, for its own turns' witnesses
            // only. Two: a condense, which has no conversation, for the witnesses of every turn in the range. A D1
            // condense whose ends happen to fall in one conversation is taken for a fold: it goes to fewer NPCs, so a
            // memory can be lost but never given to an NPC who did not witness it.
            const bool fold = first->second.conversationId == last->second.conversationId;
            const std::string conversationId = fold ? first->second.conversationId : std::string();
            std::set<std::string> npcs;
            for (auto it = first; it != turns.end() && it->first <= summary.toTurn; ++it) {
                if (!fold || it->second.conversationId == conversationId) {
                    npcs.insert(it->second.npcs.begin(), it->second.npcs.end());
                }
            }
            for (const std::string& npc : npcs) {
                db.Prepare("INSERT INTO summaries (from_turn, to_turn, text, actor_id, conversation_id) "
                           "VALUES (?1, ?2, ?3, ?4, ?5)")
                    .Bind(1, summary.fromTurn)
                    .Bind(2, summary.toTurn)
                    .Bind(3, summary.text)
                    .Bind(4, npc)
                    .Bind(5, conversationId)
                    .Run();
            }
            db.Prepare("DELETE FROM summaries WHERE id = ?1").Bind(1, summary.id).Run();
        }

        // Each NPC witness's mark: how far its conversation is folded. The conversations are read whole first too.
        std::vector<std::pair<std::string, int>> conversations;
        {
            Statement conversationQuery = db.Prepare("SELECT id, folded_through FROM conversations ORDER BY position");
            while (conversationQuery.Step()) {
                conversations.emplace_back(conversationQuery.ColumnText(0),
                                           static_cast<int>(conversationQuery.ColumnInt(1)));
            }
        }
        for (const auto& [conversationId, foldedThrough] : conversations) {
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

    // Version 6 (rollback spec, section 4): clocks and hidden marks on hosted rows, conversations as history, and a
    // superseded summary linked to its condense. Existing rows get no clock: a rewind never hides them.
    auto upgradeTo6 = [&db, &file] {
        db.Exec(V6_COLUMNS_SQL);
        db.Exec(V6_TABLES_SQL);
        // Each conversation's participants and end become its first change row.
        db.Exec("INSERT INTO conversation_changes (conversation_id, clock, participants, ended) "
                "SELECT id, NULL, participants, ended FROM conversations ORDER BY position");
        // A superseded summary is linked to its condense: the actor's first condensed summary written after it (a
        // condensed summary has no conversation and no teller). The superseded rows are read whole before any insert.
        std::vector<std::pair<std::int64_t, std::string>> rows;
        {
            Statement superseded = db.Prepare("SELECT id, actor_id FROM summaries WHERE superseded = 1 ORDER BY id");
            while (superseded.Step()) {
                rows.emplace_back(superseded.ColumnInt(0), superseded.ColumnText(1));
            }
        }
        for (const auto& [id, actor] : rows) {
            std::int64_t condenseId = 0;
            {
                Statement condense = db.Prepare("SELECT id FROM summaries WHERE actor_id = ?1 AND id > ?2 AND "
                                                "conversation_id = '' AND told_by = '' ORDER BY id LIMIT 1");
                condense.Bind(1, actor).Bind(2, id);
                if (!condense.Step()) {
                    throw StoreError(std::format("'{}': summary {}: superseded, but no later condensed summary of '{}'",
                                                 core::PathText(file), id, actor));
                }
                condenseId = condense.ColumnInt(0);
            }
            db.Prepare("INSERT INTO summary_supersessions (summary_id, condensed_id) VALUES (?1, ?2)")
                .Bind(1, id)
                .Bind(2, condenseId)
                .Run();
        }
        // Their data moved, the replaced columns go.
        db.Exec(V6_DROPPED_COLUMNS_SQL);
    };

    // An older run is upgraded in place to version 6, in one transaction, step by step: version 2 gains the hosted
    // tables (already with version 4's column) and is marked owned; version 3 gains hosted_turns.events_omitted, 0 for
    // the turns it holds; every one of them gains version 5's per-actor columns and tables, and a hosted run's
    // run-wide summaries move to their NPC witnesses; then every version up to 5 takes version 6's changes. An owned
    // run's data is untouched (its hosted tables are empty, and none of its summaries is superseded). Any other version
    // is refused below.
    const std::string version = value("schema_version"); // A copy: `values` changes below.
    if (version == "2" || version == "3" || version == "4" || version == "5") {
        try {
            // A version 2 file is owned; a later one says its kind.
            const bool hosted = version != "2" && value("kind") == RunKindName(RunKind::Hosted);
            db.Transaction([&] {
                if (version == "2") {
                    db.Exec(HOSTED_SCHEMA_SQL);
                    db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)")
                        .Bind(1, RunKindName(RunKind::Owned))
                        .Run();
                }
                if (version == "3") {
                    db.Exec(EVENTS_OMITTED_SQL);
                }
                // Versions 2 to 4 reach version 5.
                if (version != "5") {
                    db.Exec(PER_ACTOR_COLUMNS_SQL);
                    db.Exec(PER_ACTOR_TABLES_SQL);
                    if (hosted) {
                        moveSummaries();
                    }
                }
                upgradeTo6();
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
    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     core::PathText(file), value("schema_version"), SCHEMA_VERSION));
    }

    RunMeta meta;
    meta.worldId = value("world_id");
    meta.worldHash = value("world_hash");
    meta.modelId = value("model_id");

    // The seed is stored as text; accept only a complete unsigned 64-bit decimal number.
    const std::string& seedText = value("rng_seed");
    const auto [pSeedEnd, seedError] =
        std::from_chars(seedText.data(), seedText.data() + seedText.size(), meta.rngSeed);
    if (seedError != std::errc() || pSeedEnd != seedText.data() + seedText.size()) {
        throw StoreError(std::format("'{}': run_meta 'rng_seed' is not a valid seed: '{}'",
                                     core::PathText(file), seedText));
    }

    try {
        meta.settings = nlohmann::json::parse(value("settings"));
    } catch (const nlohmann::json::parse_error& error) {
        throw StoreError(std::format("'{}': run_meta 'settings' is not valid JSON: {}",
                                     core::PathText(file), error.what()));
    }
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

    RunStore store(std::move(db), std::move(meta), file);
    if (store._meta.kind == RunKind::Hosted) {
        // A hosted run's memory marks each name one of its conversations: a mark of any other is damaged saved data.
        {
            Statement orphan = store._db.Prepare("SELECT conversation_id FROM actor_memory "
                                                 "WHERE conversation_id NOT IN (SELECT id FROM conversations) LIMIT 1");
            if (orphan.Step()) {
                throw StoreError(std::format("'{}': actor_memory.conversation_id '{}' names no conversation of the run",
                                             core::PathText(file), orphan.ColumnText(0)));
            }
        }
        // Every hidden row names a rollback, and at most one rollback is open.
        for (const char* table : {"hosted_turns", "conversation_events", "conversation_changes", "conversations",
                                  "summaries", "pending_shares"}) {
            Statement dangling = store._db.Prepare(std::format(
                "SELECT COUNT(*) FROM {} WHERE hidden_by <> 0 AND hidden_by NOT IN (SELECT id FROM rollbacks)", table));
            const bool hasRow = dangling.Step();
            MORPHEUS_REQUIRE(hasRow, "RunStore::Open: aggregate query returned no row");
            if (dangling.ColumnInt(0) > 0) {
                throw StoreError(std::format("'{}': {}: hidden_by names no rollback", core::PathText(file), table));
            }
        }
        store.OpenRollback(); // Throws when more than one is open.
    }
    return store;
}

const RunMeta& RunStore::Meta() const
{
    return _meta;
}

int RunStore::LatestTurn()
{
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

nlohmann::json RunStore::StateAt(int turn)
{
    Statement query = _db.Prepare("SELECT state FROM state_snapshots WHERE turn = ?1");
    query.Bind(1, turn);
    const bool found = query.Step();
    MORPHEUS_REQUIRE(found, std::format("StateAt: no snapshot for turn {}", turn));
    try {
        return nlohmann::json::parse(query.ColumnText(0));
    } catch (const nlohmann::json::parse_error& error) {
        throw StoreError(std::format("'{}': state snapshot for turn {} is not valid JSON: {}",
                                     core::PathText(_file), turn, error.what()));
    }
}

void RunStore::CommitTurn(const core::TurnRecord& record, const nlohmann::json& stateAfter)
{
    RequireOwned("CommitTurn");
    const int expected = LatestTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitTurn: expected turn {}, got {}", expected, record.number));

    // Typed turn data is stored as JSON arrays in its own columns.
    nlohmann::json checks = nlohmann::json::array();
    for (const core::CheckOutcome& check : record.checks) {
        checks.push_back(core::CheckOutcomeToJson(check));
    }
    nlohmann::json applied = nlohmann::json::array();
    for (const core::AppliedChange& change : record.applied) {
        applied.push_back(core::AppliedChangeToJson(change));
    }
    nlohmann::json rejected = nlohmann::json::array();
    for (const core::RejectedChange& change : record.rejected) {
        rejected.push_back(core::RejectedChangeToJson(change));
    }

    // The turn, its resulting state and its notes land together or not at all.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO turns (number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                    "timings) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)")
            .Bind(1, record.number)
            .Bind(2, record.attempt)
            .Bind(3, record.input)
            .Bind(4, record.resolve.dump())
            .Bind(5, checks.dump())
            .Bind(6, applied.dump())
            .Bind(7, rejected.dump())
            .Bind(8, record.narration)
            .Bind(9, record.digest)
            .Bind(10, record.timings.dump())
            .Run();
        _db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (?1, ?2)")
            .Bind(1, record.number)
            .Bind(2, stateAfter.dump())
            .Run();

        // Notes belong to this turn: they commit with it and roll back with it.
        for (const std::string& note : record.notes) {
            _db.Prepare("INSERT INTO notes (turn, text) VALUES (?1, ?2)").Bind(1, record.number).Bind(2, note).Run();
        }
    });
}

std::optional<core::TurnRecord> RunStore::Turn(int number)
{
    std::vector<core::TurnRecord> found = Turns(number, number);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::move(found.front());
}

std::vector<core::TurnRecord> RunStore::Turns(int fromTurn, int toTurn)
{
    Statement query = _db.Prepare("SELECT number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                                  "timings FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
    query.Bind(1, fromTurn).Bind(2, toTurn);

    std::vector<core::TurnRecord> records;
    while (query.Step()) {
        core::TurnRecord record;
        record.number = static_cast<int>(query.ColumnInt(0));

        // Corrupt saved data is a StoreError naming the file, turn and column, not a raw parse exception.
        auto parseColumn = [&](int column, const char* name) {
            try {
                return nlohmann::json::parse(query.ColumnText(column));
            } catch (const nlohmann::json::parse_error& error) {
                throw StoreError(std::format("'{}': turn {} column '{}' is not valid JSON: {}",
                                             core::PathText(_file), record.number, name, error.what()));
            }
        };

        // A stored column that parses but has the wrong shape is damaged data too.
        auto convertColumn = [&](int column, const char* name, auto fromJson, auto& out) {
            const nlohmann::json array = parseColumn(column, name);
            try {
                if (!array.is_array()) {
                    throw core::DataError("not an array");
                }
                for (const nlohmann::json& element : array) {
                    out.push_back(fromJson(element));
                }
            } catch (const core::DataError& error) {
                throw StoreError(std::format("'{}': turn {} column '{}' is malformed: {}",
                                             core::PathText(_file), record.number, name, error.what()));
            }
        };

        record.attempt = static_cast<int>(query.ColumnInt(1));
        record.input = query.ColumnText(2);
        record.resolve = parseColumn(3, "resolve");
        convertColumn(4, "checks", core::CheckOutcomeFromJson, record.checks);
        convertColumn(5, "applied", core::AppliedChangeFromJson, record.applied);
        convertColumn(6, "rejected", core::RejectedChangeFromJson, record.rejected);
        record.narration = query.ColumnText(7);
        record.digest = query.ColumnText(8);
        record.timings = parseColumn(9, "timings");
        records.push_back(std::move(record));
    }

    // Attach each turn's notes, in the order they were written. Turns are ascending, so index them by number.
    std::map<int, core::TurnRecord*> byNumber;
    for (core::TurnRecord& record : records) {
        byNumber.emplace(record.number, &record);
    }
    Statement noteQuery = _db.Prepare("SELECT turn, text FROM notes WHERE turn BETWEEN ?1 AND ?2 ORDER BY id");
    noteQuery.Bind(1, fromTurn).Bind(2, toTurn);
    while (noteQuery.Step()) {
        // Notes commit with their turn, so an orphan note means the saved data was damaged.
        const int noteTurn = static_cast<int>(noteQuery.ColumnInt(0));
        const auto it = byNumber.find(noteTurn);
        if (it == byNumber.end()) {
            throw StoreError(std::format("'{}': note for turn {} has no matching turn row", core::PathText(_file), noteTurn));
        }
        it->second->notes.push_back(noteQuery.ColumnText(1));
    }
    return records;
}

void RunStore::TruncateAfter(int turn)
{
    RequireOwned("TruncateAfter");
    MORPHEUS_REQUIRE(turn >= 0, "TruncateAfter: turn must be >= 0");

    // Everything that happened after `turn` goes, in one transaction. Summaries that end at or before `turn`
    // only cover unchanged turns, so they stay.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM turns WHERE number > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM state_snapshots WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM summaries WHERE to_turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM notes WHERE turn > ?1").Bind(1, turn).Run();
        // CAST('12.0' AS INTEGER) is 12, so note refs ("<turn>.<index>") compare by their turn.
        _db.Prepare("DELETE FROM embeddings WHERE kind IN ('turn', 'note') AND CAST(ref_id AS INTEGER) > ?1")
            .Bind(1, turn)
            .Run();
    });
}

void RunStore::AddSummary(const core::Summary& summary)
{
    RequireOwned("AddSummary");
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("AddSummary: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
        .Bind(1, summary.fromTurn)
        .Bind(2, summary.toTurn)
        .Bind(3, summary.text)
        .Run();
}

void RunStore::ReplaceSummaries(const core::Summary& merged)
{
    RequireOwned("ReplaceSummaries");
    MORPHEUS_REQUIRE(merged.fromTurn >= 1 && merged.fromTurn <= merged.toTurn,
                     std::format("ReplaceSummaries: bad turn range {}..{}", merged.fromTurn, merged.toTurn));

    // The old run-wide summaries go and the merged one arrives together, so a failure never leaves the run without
    // its story so far.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM summaries WHERE actor_id = ''").Run();
        _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
            .Bind(1, merged.fromTurn)
            .Bind(2, merged.toTurn)
            .Bind(3, merged.text)
            .Run();
    });
}

std::vector<core::Summary> RunStore::Summaries()
{
    RequireOwned("Summaries");
    Statement query =
        _db.Prepare("SELECT from_turn, to_turn, text FROM summaries WHERE actor_id = '' ORDER BY from_turn, id");
    std::vector<core::Summary> summaries;
    while (query.Step()) {
        summaries.push_back({static_cast<int>(query.ColumnInt(0)), static_cast<int>(query.ColumnInt(1)),
                             query.ColumnText(2)});
    }
    return summaries;
}

std::vector<core::Note> RunStore::Notes()
{
    Statement query = _db.Prepare("SELECT turn, text FROM notes ORDER BY turn, id");
    std::vector<core::Note> notes;
    while (query.Step()) {
        notes.push_back({static_cast<int>(query.ColumnInt(0)), query.ColumnText(1)});
    }
    return notes;
}

void RunStore::PutEmbedding(const EmbeddingRow& row)
{
    MORPHEUS_REQUIRE(!row.vector.empty(), "PutEmbedding: vector must not be empty");
    MORPHEUS_REQUIRE(!row.contentHash.empty(), "PutEmbedding: contentHash must not be empty");
    _db.Prepare("INSERT OR REPLACE INTO embeddings (kind, ref_id, model, vector, content_hash) "
                "VALUES (?1, ?2, ?3, ?4, ?5)")
        .Bind(1, KindName(row.kind))
        .Bind(2, row.refId)
        .Bind(3, row.model)
        .Bind(4, row.vector)
        .Bind(5, row.contentHash)
        .Run();
}

std::vector<EmbeddingRow> RunStore::Embeddings(EmbeddingKind kind, const std::string& model)
{
    Statement query = _db.Prepare(
        "SELECT ref_id, vector, content_hash FROM embeddings WHERE kind = ?1 AND model = ?2 "
        "ORDER BY CAST(ref_id AS INTEGER), ref_id");
    query.Bind(1, KindName(kind)).Bind(2, model);

    std::vector<EmbeddingRow> rows;
    while (query.Step()) {
        rows.push_back({kind, query.ColumnText(0), model, query.ColumnFloats(1), query.ColumnText(2)});
    }
    return rows;
}

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

std::string RunStore::CreateConversation(const std::vector<core::Participant>& participants,
                                         std::optional<double> clock)
{
    RequireHosted("CreateConversation");
    RequireNoOpenRollback("CreateConversation");
    MORPHEUS_REQUIRE(!participants.empty(), "CreateConversation: a conversation needs a participant");
    // Conversations are never deleted (a hidden one stays as a row), so their count names the next one: c1, c2, ...
    // The count's statement is finished before the insert.
    std::int64_t position = 0;
    {
        Statement count = _db.Prepare("SELECT COUNT(*) FROM conversations");
        const bool hasRow = count.Step();
        MORPHEUS_REQUIRE(hasRow, "CreateConversation: aggregate query returned no row");
        position = count.ColumnInt(0) + 1;
    }
    const std::string id = std::format("c{}", position);
    // The start row and the first change row (its participants, not ended) land together: a conversation is never
    // without its participants.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO conversations (id, position, folded_through, clock) VALUES (?1, ?2, 0, ?3)")
            .Bind(1, id)
            .Bind(2, position)
            .Bind(3, clock)
            .Run();
        _db.Prepare("INSERT INTO conversation_changes (conversation_id, clock, participants, ended) "
                    "VALUES (?1, ?2, ?3, 0)")
            .Bind(1, id)
            .Bind(2, clock)
            .Bind(3, ParticipantsToJson(participants).dump())
            .Run();
    });
    return id;
}

std::optional<core::Conversation> RunStore::Conversation(std::string_view id)
{
    RequireHosted("Conversation");
    // The one visible start row, then its latest visible change row.
    Statement query =
        _db.Prepare(std::format("SELECT {} FROM conversations WHERE id = ?1 AND hidden_by = 0", CONVERSATION_COLUMNS));
    query.Bind(1, id);
    if (!query.Step()) {
        return std::nullopt;
    }
    return ReadConversation(query);
}

std::vector<core::Conversation> RunStore::Conversations()
{
    RequireHosted("Conversations");
    // The visible start rows in creation order, each read with its latest visible change row.
    Statement query = _db.Prepare(
        std::format("SELECT {} FROM conversations WHERE hidden_by = 0 ORDER BY position", CONVERSATION_COLUMNS));
    std::vector<core::Conversation> conversations;
    while (query.Step()) {
        conversations.push_back(ReadConversation(query));
    }
    return conversations;
}

void RunStore::SetParticipants(std::string_view id, const std::vector<core::Participant>& participants,
                               std::optional<double> clock)
{
    RequireHosted("SetParticipants");
    RequireNoOpenRollback("SetParticipants");
    const std::optional<core::Conversation> conversation = Conversation(id);
    MORPHEUS_REQUIRE(conversation.has_value(), "SetParticipants: no such conversation");
    MORPHEUS_REQUIRE(!participants.empty(), "SetParticipants: a conversation needs a participant");
    // A change row with the new participants; the end stays as it is.
    _db.Prepare("INSERT INTO conversation_changes (conversation_id, clock, participants, ended) VALUES (?1, ?2, ?3, ?4)")
        .Bind(1, id)
        .Bind(2, clock)
        .Bind(3, ParticipantsToJson(participants).dump())
        .Bind(4, conversation->ended ? 1 : 0)
        .Run();
}

void RunStore::EndConversation(std::string_view id, std::optional<double> clock)
{
    RequireHosted("EndConversation");
    RequireNoOpenRollback("EndConversation");
    const std::optional<core::Conversation> conversation = Conversation(id);
    MORPHEUS_REQUIRE(conversation.has_value(), "EndConversation: no such conversation");
    // A change row that ends it; the participants stay as they are.
    _db.Prepare("INSERT INTO conversation_changes (conversation_id, clock, participants, ended) VALUES (?1, ?2, ?3, 1)")
        .Bind(1, id)
        .Bind(2, clock)
        .Bind(3, ParticipantsToJson(conversation->participants).dump())
        .Run();
}

std::int64_t RunStore::FoldConversation(std::string_view id, const core::Summary& summary)
{
    RequireHosted("FoldConversation");
    MORPHEUS_REQUIRE(!summary.actorId.empty(), "FoldConversation: the summary needs its actor");
    MORPHEUS_REQUIRE(summary.conversationId == id,
                     std::format("FoldConversation: the summary is of conversation '{}', not '{}'",
                                 summary.conversationId, id));
    // The summary, the conversation's folded mark and the actor's own mark land together: a turn is never both folded
    // and shown in L3. Each step checks its own contract (the turn range, the marks only moving forward and never past
    // a turn that exists); a failed one rolls the others back.
    std::int64_t summaryId = 0;
    _db.Transaction([&] {
        summaryId = AddActorSummary(summary);
        MoveFoldedThrough("FoldConversation", id, summary.toTurn);
        SetRememberedThrough(id, summary.actorId, summary.toTurn);
    });
    return summaryId;
}

void RunStore::MarkFolded(std::string_view id, int turn)
{
    RequireHosted("MarkFolded");
    MoveFoldedThrough("MarkFolded", id, turn);
}

std::vector<core::Summary> RunStore::ActorSummaries(std::string_view actorId, bool includeSuperseded)
{
    RequireHosted("ActorSummaries");
    MORPHEUS_REQUIRE(!actorId.empty(), "ActorSummaries: an actor is needed");
    // The actor's visible rows, the superseded ones (by a visible condense) only when asked for, oldest range first.
    Statement query = _db.Prepare(
        std::format("SELECT {} FROM summaries s WHERE s.actor_id = ?1 AND s.hidden_by = 0{} ORDER BY s.from_turn, s.id",
                    SUMMARY_COLUMNS, includeSuperseded ? "" : " AND NOT superseded"));
    query.Bind(1, actorId);
    std::vector<core::Summary> summaries;
    while (query.Step()) {
        summaries.push_back(ReadSummaryRow(query));
    }
    return summaries;
}

std::vector<core::Summary> RunStore::HostedSummaries()
{
    RequireHosted("HostedSummaries");
    // Every visible row: in a hosted run each one has its actor, and ReadSummaryRow refuses one without.
    Statement query = _db.Prepare(
        std::format("SELECT {} FROM summaries s WHERE s.hidden_by = 0 ORDER BY s.from_turn, s.id", SUMMARY_COLUMNS));
    std::vector<core::Summary> summaries;
    while (query.Step()) {
        summaries.push_back(ReadSummaryRow(query));
    }
    return summaries;
}

std::int64_t RunStore::AddActorSummary(const core::Summary& summary)
{
    RequireHosted("AddActorSummary");
    MORPHEUS_REQUIRE(!summary.actorId.empty(), "AddActorSummary: the summary needs its actor");
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("AddActorSummary: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    // A summary written while a rollback is open is made in it: undoing the rollback deletes it.
    const std::int64_t madeIn = OpenRollbackId();
    // Every column but the id, which SQLite assigns and the caller gets back, and hidden_by (a new row is visible).
    // Callers that change more than this row hold the transaction, so the open rollback read above is that of the
    // write.
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text, actor_id, conversation_id, told_by, source_id, "
                "made_in, clock) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)")
        .Bind(1, summary.fromTurn)
        .Bind(2, summary.toTurn)
        .Bind(3, summary.text)
        .Bind(4, summary.actorId)
        .Bind(5, summary.conversationId)
        .Bind(6, summary.toldBy)
        .Bind(7, summary.sourceId)
        .Bind(8, madeIn)
        .Bind(9, summary.clock)
        .Run();
    Statement inserted = _db.Prepare("SELECT last_insert_rowid()");
    const bool hasRow = inserted.Step();
    MORPHEUS_REQUIRE(hasRow, "AddActorSummary: last_insert_rowid returned no row");
    return inserted.ColumnInt(0);
}

std::int64_t RunStore::RememberFold(const core::Summary& summary)
{
    RequireHosted("RememberFold");
    MORPHEUS_REQUIRE(!summary.conversationId.empty(), "RememberFold: the summary needs its conversation");
    // The summary and the actor's mark land together; the conversation's folded mark is the speaker's to move.
    std::int64_t summaryId = 0;
    _db.Transaction([&] {
        summaryId = AddActorSummary(summary);
        SetRememberedThrough(summary.conversationId, summary.actorId, summary.toTurn);
    });
    return summaryId;
}

std::int64_t RunStore::CondenseActorSummaries(std::string_view actorId, const core::Summary& condensed)
{
    RequireHosted("CondenseActorSummaries");
    MORPHEUS_REQUIRE(condensed.actorId == actorId,
                     std::format("CondenseActorSummaries: the condensed summary is '{}''s, not '{}''s",
                                 condensed.actorId, actorId));
    MORPHEUS_REQUIRE(condensed.conversationId.empty() && condensed.toldBy.empty(),
                     "CondenseActorSummaries: a condensed summary has no conversation and no teller");
    // The condensed summary arrives and the shown ones name it as their condense (kept, retrieved, found by shares),
    // together, so the actor never shows both or neither. Shown: visible, and not superseded by a visible summary (one
    // whose condense a rollback hid is shown again, and is condensed now).
    std::int64_t summaryId = 0;
    _db.Transaction([&] {
        summaryId = AddActorSummary(condensed);
        _db.Prepare("INSERT INTO summary_supersessions (summary_id, condensed_id) "
                    "SELECT s.id, ?2 FROM summaries s WHERE s.actor_id = ?1 AND s.id <> ?2 AND s.hidden_by = 0 AND "
                    "NOT EXISTS (SELECT 1 FROM summary_supersessions l JOIN summaries c ON c.id = l.condensed_id "
                    "WHERE l.summary_id = s.id AND c.hidden_by = 0)")
            .Bind(1, actorId)
            .Bind(2, summaryId)
            .Run();
    });
    return summaryId;
}

int RunStore::RememberedThrough(std::string_view conversationId, std::string_view actorId)
{
    RequireHosted("RememberedThrough");
    Statement query = _db.Prepare(
        "SELECT remembered_through FROM actor_memory WHERE conversation_id = ?1 AND actor_id = ?2");
    query.Bind(1, conversationId).Bind(2, actorId);
    // No row: the actor remembers none of the conversation yet.
    return query.Step() ? static_cast<int>(query.ColumnInt(0)) : 0;
}

void RunStore::SetRememberedThrough(std::string_view conversationId, std::string_view actorId, int turn)
{
    RequireHosted("SetRememberedThrough");
    MORPHEUS_REQUIRE(!actorId.empty(), "SetRememberedThrough: an actor is needed");
    MORPHEUS_REQUIRE(Conversation(conversationId).has_value(), "SetRememberedThrough: no such conversation");
    // The mark only moves forward.
    const int current = RememberedThrough(conversationId, actorId);
    MORPHEUS_REQUIRE(turn > current, std::format("SetRememberedThrough: '{}' already remembers '{}' through turn {}, "
                                                 "not before {}",
                                                 actorId, conversationId, current, turn));
    _db.Prepare("INSERT INTO actor_memory (conversation_id, actor_id, remembered_through) VALUES (?1, ?2, ?3) "
                "ON CONFLICT(conversation_id, actor_id) DO UPDATE SET remembered_through = excluded.remembered_through")
        .Bind(1, conversationId)
        .Bind(2, actorId)
        .Bind(3, turn)
        .Run();
}

std::int64_t RunStore::AddPendingShare(const core::PendingShare& share)
{
    RequireHosted("AddPendingShare");
    RequireNoOpenRollback("AddPendingShare");
    MORPHEUS_REQUIRE(!share.fromActor.empty() && !share.toActor.empty() && !share.conversationId.empty(),
                     "AddPendingShare: a share needs its teller, its listener and its conversation");
    // The id SQLite assigns is the share's, read back on the same connection right after the insert. It is pending: not
    // applied, no applied clock.
    _db.Prepare("INSERT INTO pending_shares (from_actor, to_actor, conversation_id, clock) VALUES (?1, ?2, ?3, ?4)")
        .Bind(1, share.fromActor)
        .Bind(2, share.toActor)
        .Bind(3, share.conversationId)
        .Bind(4, share.clock)
        .Run();
    Statement inserted = _db.Prepare("SELECT last_insert_rowid()");
    const bool hasRow = inserted.Step();
    MORPHEUS_REQUIRE(hasRow, "AddPendingShare: last_insert_rowid returned no row");
    return inserted.ColumnInt(0);
}

std::vector<core::PendingShare> RunStore::PendingShares()
{
    RequireHosted("PendingShares");
    // Visible shares not applied yet, or applied and then unapplied by a rollback.
    Statement query = _db.Prepare("SELECT id, from_actor, to_actor, conversation_id, clock, applied_clock, unapplied_by "
                                  "FROM pending_shares WHERE hidden_by = 0 AND (applied = 0 OR unapplied_by <> 0) "
                                  "ORDER BY id");
    std::vector<core::PendingShare> shares;
    while (query.Step()) {
        core::PendingShare share;
        share.id = query.ColumnInt(0);
        share.fromActor = query.ColumnText(1);
        share.toActor = query.ColumnText(2);
        share.conversationId = query.ColumnText(3);
        if (!query.ColumnIsNull(4)) {
            share.clock = query.ColumnDouble(4);
        }
        if (!query.ColumnIsNull(5)) {
            share.appliedClock = query.ColumnDouble(5);
        }
        share.unappliedBy = query.ColumnInt(6);
        shares.push_back(std::move(share));
    }
    return shares;
}

void RunStore::MarkShareApplied(std::int64_t id, std::optional<double> clock)
{
    RequireHosted("MarkShareApplied");
    // The row stays, so a rollback to before `clock` can make it pending again. It is applied even with no clock (a run
    // without one); such a share is never unapplied.
    _db.Prepare("UPDATE pending_shares SET applied = 1, applied_clock = ?2, unapplied_by = 0 WHERE id = ?1")
        .Bind(1, id)
        .Bind(2, clock)
        .Run();
}

void RunStore::AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events,
                                     std::optional<double> clock)
{
    RequireHosted("AddConversationEvents");
    RequireNoOpenRollback("AddConversationEvents");
    MORPHEUS_REQUIRE(Conversation(id).has_value(), "AddConversationEvents: no such conversation");
    _db.Transaction([&] {
        for (const nlohmann::json& event : events) {
            _db.Prepare("INSERT INTO conversation_events (conversation_id, after_turn, event, clock) "
                        "VALUES (?1, ?2, ?3, ?4)")
                .Bind(1, id)
                .Bind(2, afterTurn)
                .Bind(3, event.dump())
                .Bind(4, clock)
                .Run();
        }
    });
}

std::vector<core::ConversationEvent> RunStore::ConversationEvents(std::string_view id)
{
    RequireHosted("ConversationEvents");
    Statement query = _db.Prepare("SELECT after_turn, event, clock FROM conversation_events WHERE conversation_id = ?1 "
                                  "AND hidden_by = 0 ORDER BY id");
    query.Bind(1, id);
    std::vector<core::ConversationEvent> events;
    while (query.Step()) {
        core::ConversationEvent event;
        event.afterTurn = static_cast<int>(query.ColumnInt(0));
        if (!query.ColumnIsNull(2)) {
            event.clock = query.ColumnDouble(2);
        }
        try {
            event.event = nlohmann::json::parse(query.ColumnText(1));
        } catch (const nlohmann::json::parse_error& error) {
            throw StoreError(std::format("'{}': an event of conversation '{}' is not valid JSON: {}",
                                         core::PathText(_file), id, error.what()));
        }
        if (!EventIsWhole(event.event)) {
            throw StoreError(std::format("'{}': an event of conversation '{}' is not an object with a string text",
                                         core::PathText(_file), id));
        }
        events.push_back(std::move(event));
    }
    return events;
}

int RunStore::LatestHostedTurn()
{
    RequireHosted("LatestHostedTurn");
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM hosted_turns WHERE hidden_by = 0");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestHostedTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

int RunStore::NextHostedTurn()
{
    RequireHosted("NextHostedTurn");
    // Every row, hidden or not: a hidden turn's number is never given again.
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) + 1 FROM hosted_turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "NextHostedTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

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

void RunStore::CommitHostedTurn(const core::HostedTurn& record)
{
    RequireHosted("CommitHostedTurn");
    RequireNoOpenRollback("CommitHostedTurn");
    // Turn numbers are never reused: the next is one past every turn stored, hidden ones included.
    const int expected = NextHostedTurn();
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitHostedTurn: expected turn {}, got {}", expected, record.number));
    MORPHEUS_REQUIRE(Conversation(record.conversationId).has_value(), "CommitHostedTurn: no such conversation");
    MORPHEUS_REQUIRE(record.eventsOmitted >= 0, "RunStore::CommitHostedTurn: eventsOmitted must not be negative");

    // JSON columns. DumpText keeps text that is not UTF-8 from throwing.
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
                "observation, resolve_output, adjudication, line, timings, events_omitted, clock) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)")
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
        .Bind(13, record.clock)
        .Run();
}

std::vector<core::HostedTurn> RunStore::HostedTurns(std::string_view conversationId)
{
    RequireHosted("HostedTurns");
    Statement query = _db.Prepare("SELECT number, speaker_id, listener_ids, player_text, events, observation, "
                                  "resolve_output, adjudication, line, timings, events_omitted, clock FROM hosted_turns "
                                  "WHERE conversation_id = ?1 AND hidden_by = 0 ORDER BY number");
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
            if (!EventIsWhole(event)) {
                throw damaged("events", "holds an event that is not an object with a string text");
            }
            turn.events.push_back(event);
        }
        turn.observation = parseColumn(5, "observation");
        if (!turn.observation.is_object()) {
            throw damaged("observation", "is not an object");
        }
        // The actors core a hosted session reads back from every stored turn, read by the same rules as
        // HostedWorld::Observe (core::ReadActors): each fault is named by its pointer. The speaker is one of them.
        const core::ActorsCore actors = core::ReadActors(turn.observation);
        if (!actors.faults.empty()) {
            std::string faults;
            for (const core::ObservationFault& fault : actors.faults) {
                faults += std::format("{}at {}: {}", faults.empty() ? "" : "; ",
                                      fault.pointer.empty() ? "the root" : fault.pointer, fault.problem);
            }
            throw damaged("observation", "does not hold the actors core: " + faults);
        }
        const bool speakerPresent =
            std::any_of(actors.actors.begin(), actors.actors.end(),
                        [&turn](const core::Actor& actor) { return actor.id == turn.speakerId; });
        if (!speakerPresent) {
            throw damaged("speaker_id", std::format("names '{}', who is not an actor of the turn's observation",
                                                    turn.speakerId));
        }
        turn.resolveOutput = query.ColumnText(6);

        const nlohmann::json adjudication = parseColumn(7, "adjudication");
        const bool whole = adjudication.is_object() && adjudication.contains("requested") &&
                           adjudication.at("requested").is_boolean() && adjudication.contains("action_id") &&
                           adjudication.at("action_id").is_string() && adjudication.contains("params") &&
                           adjudication.at("params").is_object() &&
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
        turn.eventsOmitted = static_cast<int>(query.ColumnInt(10));
        if (turn.eventsOmitted < 0) {
            throw damaged("events_omitted", "is negative");
        }
        if (!query.ColumnIsNull(11)) {
            turn.clock = query.ColumnDouble(11);
        }
        turns.push_back(std::move(turn));
    }
    return turns;
}

std::optional<Rollback> RunStore::OpenRollback()
{
    RequireHosted("OpenRollback");
    // At most one rollback is open at a time: a second one is damaged saved data.
    Statement query = _db.Prepare("SELECT id, game_time FROM rollbacks WHERE status = 'open' ORDER BY id");
    std::optional<Rollback> open;
    while (query.Step()) {
        if (open) {
            throw StoreError(std::format("'{}': rollbacks holds more than one open rollback ({} and {})",
                                         core::PathText(_file), open->id, query.ColumnInt(0)));
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
        // What it changes, saved first so an undo restores it exactly: every conversation's folded mark, every NPC's
        // mark (as [conversation, actor, through]) and every share's application (as [id, applied, applied_clock,
        // unapplied_by]; shares applied while it is open are put back too).
        nlohmann::json saved = {{"folded_through", nlohmann::json::object()},
                                {"remembered", nlohmann::json::array()},
                                {"shares", nlohmann::json::array()}};
        {
            Statement folded = _db.Prepare("SELECT id, folded_through FROM conversations");
            while (folded.Step()) {
                saved["folded_through"][folded.ColumnText(0)] = folded.ColumnInt(1);
            }
        }
        {
            Statement remembered = _db.Prepare("SELECT conversation_id, actor_id, remembered_through FROM actor_memory");
            while (remembered.Step()) {
                saved["remembered"].push_back(
                    {remembered.ColumnText(0), remembered.ColumnText(1), remembered.ColumnInt(2)});
            }
        }
        {
            Statement shares = _db.Prepare("SELECT id, applied, applied_clock, unapplied_by FROM pending_shares");
            while (shares.Step()) {
                const nlohmann::json appliedClock =
                    shares.ColumnIsNull(2) ? nlohmann::json(nullptr) : nlohmann::json(shares.ColumnDouble(2));
                saved["shares"].push_back({shares.ColumnInt(0), shares.ColumnInt(1), appliedClock, shares.ColumnInt(3)});
            }
        }
        _db.Prepare("INSERT INTO rollbacks (game_time, status, saved_marks) VALUES (?1, 'open', ?2)")
            .Bind(1, gameTime)
            .Bind(2, saved.dump())
            .Run();
        std::int64_t id = 0;
        {
            Statement inserted = _db.Prepare("SELECT last_insert_rowid()");
            const bool hasRow = inserted.Step();
            MORPHEUS_REQUIRE(hasRow, "ApplyRollback: last_insert_rowid returned no row");
            id = inserted.ColumnInt(0);
        }

        // Records clocked after the time, and conversations started after it with everything in them (their rows all
        // have later clocks). The visible turns are counted before they are hidden.
        {
            Statement hiddenTurns = _db.Prepare("SELECT COUNT(*) FROM hosted_turns WHERE hidden_by = 0 AND clock > ?1");
            hiddenTurns.Bind(1, gameTime);
            const bool hasRow = hiddenTurns.Step();
            MORPHEUS_REQUIRE(hasRow, "ApplyRollback: aggregate query returned no row");
            counts.hiddenTurns = static_cast<int>(hiddenTurns.ColumnInt(0));
        }
        for (const char* table : {"hosted_turns", "conversation_events", "conversation_changes", "conversations"}) {
            _db.Prepare(std::format("UPDATE {} SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2", table))
                .Bind(1, id)
                .Bind(2, gameTime)
                .Run();
        }
        // Shared copies and pending shares made after it; shares applied after it are pending again (a share hidden
        // here is not also unapplied: it is gone until an undo, applied or not).
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2")
            .Bind(1, id)
            .Bind(2, gameTime)
            .Run();
        _db.Prepare("UPDATE pending_shares SET hidden_by = ?1 WHERE hidden_by = 0 AND clock > ?2")
            .Bind(1, id)
            .Bind(2, gameTime)
            .Run();
        _db.Prepare("UPDATE pending_shares SET unapplied_by = ?1 WHERE hidden_by = 0 AND applied = 1 AND unapplied_by = 0 "
                    "AND applied_clock > ?2")
            .Bind(1, id)
            .Bind(2, gameTime)
            .Run();
        // Summaries covering a hidden turn: past their conversation's last visible turn, or, with no conversation (a
        // condense), past the run's.
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND conversation_id <> '' AND to_turn > "
                    "COALESCE((SELECT MAX(number) FROM hosted_turns t WHERE t.conversation_id = summaries.conversation_id "
                    "AND t.hidden_by = 0), 0)")
            .Bind(1, id)
            .Run();
        _db.Prepare("UPDATE summaries SET hidden_by = ?1 WHERE hidden_by = 0 AND conversation_id = '' AND to_turn > "
                    "COALESCE((SELECT MAX(number) FROM hosted_turns WHERE hidden_by = 0), 0)")
            .Bind(1, id)
            .Run();
        // Marks back to each conversation's last visible turn (two-argument MIN is the scalar minimum).
        _db.Prepare("UPDATE conversations SET folded_through = MIN(folded_through, COALESCE((SELECT MAX(number) FROM "
                    "hosted_turns t WHERE t.conversation_id = conversations.id AND t.hidden_by = 0), 0))")
            .Run();
        _db.Prepare("UPDATE actor_memory SET remembered_through = MIN(remembered_through, COALESCE((SELECT MAX(number) "
                    "FROM hosted_turns t WHERE t.conversation_id = actor_memory.conversation_id AND t.hidden_by = 0), 0))")
            .Run();
        // And an NPC's mark before the first turn of its own fold summaries of that conversation (not a shared copy:
        // told_by empty; not a condense: conversation set) that this rollback hid and that cover a visible turn: the
        // background catch-up refolds those turns. A hidden fold ends past the last visible turn, so it covers one
        // exactly when it starts at or before it. With none, the mark stays.
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

    // The saved marks, read and checked whole before anything changes: damaged ones stop the undo with nothing changed.
    nlohmann::json saved;
    {
        Statement marks = _db.Prepare("SELECT saved_marks FROM rollbacks WHERE id = ?1");
        marks.Bind(1, open->id);
        const bool hasRow = marks.Step();
        MORPHEUS_REQUIRE(hasRow, "UndoRollback: the open rollback's row is gone");
        saved = nlohmann::json::parse(marks.ColumnText(0), nullptr, false);
    }
    auto damaged = [&](const char* problem) {
        return StoreError(std::format("'{}': rollback {}: rollbacks column 'saved_marks' {}", core::PathText(_file),
                                      open->id, problem));
    };
    if (saved.is_discarded()) {
        throw damaged("is not valid JSON");
    }
    if (!saved.is_object()) {
        throw damaged("is not an object");
    }
    // folded_through: {conversation: through}.
    if (!saved.contains("folded_through") || !saved.at("folded_through").is_object()) {
        throw damaged("has no folded_through object");
    }
    for (const nlohmann::json& through : saved.at("folded_through")) {
        if (!through.is_number_integer()) {
            throw damaged("holds a folded_through that is not an integer");
        }
    }
    // remembered: [[conversation, actor, through], ...].
    if (!saved.contains("remembered") || !saved.at("remembered").is_array()) {
        throw damaged("has no remembered array");
    }
    for (const nlohmann::json& mark : saved.at("remembered")) {
        const bool whole = mark.is_array() && mark.size() == 3 && mark.at(0).is_string() && mark.at(1).is_string() &&
                           mark.at(2).is_number_integer();
        if (!whole) {
            throw damaged("holds a remembered mark that is not [conversation, actor, through]");
        }
    }
    // shares: [[id, applied, applied_clock or null, unapplied_by], ...].
    if (!saved.contains("shares") || !saved.at("shares").is_array()) {
        throw damaged("has no shares array");
    }
    for (const nlohmann::json& share : saved.at("shares")) {
        const bool whole = share.is_array() && share.size() == 4 && share.at(0).is_number_integer() &&
                           share.at(1).is_number_integer() && (share.at(2).is_number() || share.at(2).is_null()) &&
                           share.at(3).is_number_integer();
        if (!whole) {
            throw damaged("holds a share that is not [id, applied, applied_clock, unapplied_by]");
        }
    }

    RollbackCounts counts;
    _db.Transaction([&] {
        {
            Statement restored = _db.Prepare("SELECT COUNT(*) FROM hosted_turns WHERE hidden_by = ?1");
            restored.Bind(1, open->id);
            const bool hasRow = restored.Step();
            MORPHEUS_REQUIRE(hasRow, "UndoRollback: aggregate query returned no row");
            counts.restoredTurns = static_cast<int>(restored.ColumnInt(0));
        }
        // Summaries made while it was open go, with their vectors and their condenses' supersession links (a summary
        // made in it is only ever condensed by a condense made in it); what it hid comes back.
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
        // Every share's application as saved: those it unapplied are applied again, and those applied while it was
        // open (their copies deleted above) are as they were. A new share abandons the rollback first, so no share was
        // added while it was open.
        for (const nlohmann::json& share : saved.at("shares")) {
            const std::optional<double> appliedClock =
                share.at(2).is_null() ? std::nullopt : std::optional<double>(share.at(2).get<double>());
            _db.Prepare("UPDATE pending_shares SET applied = ?2, applied_clock = ?3, unapplied_by = ?4 WHERE id = ?1")
                .Bind(1, share.at(0).get<std::int64_t>())
                .Bind(2, share.at(1).get<std::int64_t>())
                .Bind(3, appliedClock)
                .Bind(4, share.at(3).get<std::int64_t>())
                .Run();
        }
        // The saved marks, exactly.
        const nlohmann::json& foldedThrough = saved.at("folded_through");
        for (auto it = foldedThrough.begin(); it != foldedThrough.end(); ++it) {
            _db.Prepare("UPDATE conversations SET folded_through = ?1 WHERE id = ?2")
                .Bind(1, it.value().get<std::int64_t>())
                .Bind(2, it.key())
                .Run();
        }
        _db.Prepare("DELETE FROM actor_memory").Run();
        for (const nlohmann::json& mark : saved.at("remembered")) {
            _db.Prepare("INSERT INTO actor_memory (conversation_id, actor_id, remembered_through) VALUES (?1, ?2, ?3)")
                .Bind(1, mark.at(0).get<std::string>())
                .Bind(2, mark.at(1).get<std::string>())
                .Bind(3, mark.at(2).get<std::int64_t>())
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

void RunStore::RequireOwned(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Owned, std::format("RunStore::{}: this is a hosted run", method));
}

void RunStore::RequireHosted(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Hosted, std::format("RunStore::{}: this is an owned run", method));
}

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
    summary.supersededBy = query.ColumnInt(8);
    summary.madeIn = query.ColumnInt(9);
    if (!query.ColumnIsNull(10)) {
        summary.clock = query.ColumnDouble(10);
    }
    summary.superseded = query.ColumnInt(11) != 0;
    // Every hosted summary is some NPC's: one without its actor is damaged saved data.
    if (summary.actorId.empty()) {
        throw StoreError(std::format("'{}': summary {}: actor_id is empty in a hosted run", core::PathText(_file),
                                     summary.id));
    }
    return summary;
}

core::Conversation RunStore::ReadConversation(Statement& startRow)
{
    // The start row: id, folded_through and the clock it started at.
    core::Conversation conversation;
    conversation.id = startRow.ColumnText(0);
    conversation.foldedThrough = static_cast<int>(startRow.ColumnInt(1));
    if (!startRow.ColumnIsNull(2)) {
        conversation.clock = startRow.ColumnDouble(2);
    }

    // The latest visible change row holds the participants and the end now. Every conversation starts with one, hidden
    // only with its start row: a visible conversation without one is damaged saved data.
    Statement query = _db.Prepare("SELECT participants, ended FROM conversation_changes WHERE conversation_id = ?1 "
                                  "AND hidden_by = 0 ORDER BY id DESC LIMIT 1");
    query.Bind(1, conversation.id);
    if (!query.Step()) {
        throw StoreError(std::format("'{}': conversation '{}' has no visible row in conversation_changes",
                                     core::PathText(_file), conversation.id));
    }
    // Damaged participants are a StoreError naming the file, the conversation and the column.
    auto damaged = [&](const std::string& problem) {
        return StoreError(std::format("'{}': conversation '{}' conversation_changes column 'participants' {}",
                                      core::PathText(_file), conversation.id, problem));
    };
    nlohmann::json participants;
    try {
        participants = nlohmann::json::parse(query.ColumnText(0));
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
    conversation.ended = query.ColumnInt(1) != 0;
    return conversation;
}

std::int64_t RunStore::OpenRollbackId()
{
    // OpenRollback is the one reader of the open rollback, and its check of more than one open.
    const std::optional<Rollback> open = OpenRollback();
    return open ? open->id : 0;
}

void RunStore::RequireNoOpenRollback(const char* method)
{
    // A record written while a rollback is open would outlive its undo, or be hidden with nothing to show it again.
    MORPHEUS_REQUIRE(OpenRollbackId() == 0,
                     std::format("RunStore::{}: a rollback is open; abandon it before storing a new record", method));
}

void RunStore::MoveFoldedThrough(const char* method, std::string_view id, int turn)
{
    const std::optional<core::Conversation> conversation = Conversation(id);
    MORPHEUS_REQUIRE(conversation.has_value(), std::format("{}: no such conversation", method));
    // The folded mark only moves forward, and never past a turn that exists.
    MORPHEUS_REQUIRE(turn > conversation->foldedThrough,
                     std::format("{}: turn {} is already folded through {}", method, turn,
                                 conversation->foldedThrough));
    const int latest = LatestHostedTurn();
    MORPHEUS_REQUIRE(turn <= latest, std::format("{}: turn {} is past the latest turn {}", method, turn, latest));
    _db.Prepare("UPDATE conversations SET folded_through = ?1 WHERE id = ?2").Bind(1, turn).Bind(2, id).Run();
}

RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)
    : _db(std::move(db))
    , _meta(std::move(meta))
    , _file(std::move(file))
{
}

} // namespace morpheus::store
