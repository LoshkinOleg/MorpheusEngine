#include "store/RunStore.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
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
CREATE INDEX hosted_turns_conversation ON hosted_turns (conversation_id);
CREATE INDEX conversation_events_conversation ON conversation_events (conversation_id);
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

    // A version 2 run is upgraded in place: the hosted tables are added and it is marked owned, in one transaction.
    // Its data is not touched. Any other version is refused.
    if (value("schema_version") == "2") {
        try {
            db.Transaction([&] {
                db.Exec(HOSTED_SCHEMA_SQL);
                db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)")
                    .Bind(1, RunKindName(RunKind::Owned))
                    .Run();
                db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'schema_version'")
                    .Bind(1, std::to_string(SCHEMA_VERSION))
                    .Run();
            });
        } catch (const StoreError& error) {
            // The transaction rolled back: the file is still a version 2 run.
            throw StoreError(std::format("'{}': cannot upgrade from schema version 2 to {}: {}", core::PathText(file),
                                         SCHEMA_VERSION, error.what()));
        }
        values["schema_version"] = std::to_string(SCHEMA_VERSION);
        values["kind"] = RunKindName(RunKind::Owned);
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
    return RunStore(std::move(db), std::move(meta), file);
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
    MORPHEUS_REQUIRE(merged.fromTurn >= 1 && merged.fromTurn <= merged.toTurn,
                     std::format("ReplaceSummaries: bad turn range {}..{}", merged.fromTurn, merged.toTurn));

    // The old summaries go and the merged one arrives together, so a failure never leaves the run without
    // its story so far.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM summaries").Run();
        _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
            .Bind(1, merged.fromTurn)
            .Bind(2, merged.toTurn)
            .Bind(3, merged.text)
            .Run();
    });
}

std::vector<core::Summary> RunStore::Summaries()
{
    Statement query = _db.Prepare("SELECT from_turn, to_turn, text FROM summaries ORDER BY from_turn, id");
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
    const std::optional<core::Conversation> conversation = Conversation(id);
    MORPHEUS_REQUIRE(conversation.has_value(), "FoldConversation: no such conversation");
    // The folded mark only moves forward, and never past a turn that exists.
    MORPHEUS_REQUIRE(summary.toTurn > conversation->foldedThrough,
                     std::format("FoldConversation: turn {} is already folded through {}", summary.toTurn,
                                 conversation->foldedThrough));
    MORPHEUS_REQUIRE(summary.toTurn <= LatestHostedTurn(),
                     std::format("FoldConversation: turn {} is past the latest turn {}", summary.toTurn,
                                 LatestHostedTurn()));
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
                "observation, resolve_output, adjudication, line, timings) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)")
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
        turns.push_back(std::move(turn));
    }
    return turns;
}

void RunStore::RequireOwned(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Owned, std::format("RunStore::{}: this is a hosted run", method));
}

void RunStore::RequireHosted(const char* method) const
{
    MORPHEUS_REQUIRE(_meta.kind == RunKind::Hosted, std::format("RunStore::{}: this is an owned run", method));
}

RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)
    : _db(std::move(db))
    , _meta(std::move(meta))
    , _file(std::move(file))
{
}

} // namespace morpheus::store
