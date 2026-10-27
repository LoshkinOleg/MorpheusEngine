#include "store/RunStore.h"

#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <map>
#include <utility>

#include "core/Errors.h"

namespace morpheus::store {

namespace {

constexpr const char* SCHEMA_SQL = R"sql(
CREATE TABLE run_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    rolls     TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
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

// Stored spelling of an embedding kind. TruncateAfter's SQL relies on "turn".
const char* KindName(EmbeddingKind kind)
{
    switch (kind) {
    case EmbeddingKind::Lore:
        return "lore";
    case EmbeddingKind::Turn:
        return "turn";
    }
    throw morpheus::core::ContractError("KindName: unhandled EmbeddingKind");
}

} // namespace

RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)
{
    if (std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' already exists", file.generic_string()));
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    meta.createdAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                 std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Opening creates the (empty) file, so any failure from here on must remove it again, or a retry would be
    // blocked by "already exists".
    try {
        // Schema, meta and the turn-0 snapshot land together or not at all.
        Database db = Database::Open(file, true);
        db.Transaction([&] {
            db.Exec(SCHEMA_SQL);

            const std::array<std::pair<const char*, std::string>, 7> metaRows = {{
                {"schema_version", std::to_string(SCHEMA_VERSION)},
                {"world_id", meta.worldId},
                {"world_hash", meta.worldHash},
                {"model_id", meta.modelId},
                {"rng_seed", std::to_string(meta.rngSeed)}, // Text: SQLite integers are signed 64-bit.
                {"settings", meta.settings.dump()},
                {"created_at", meta.createdAt},
            }};
            for (const auto& [key, value] : metaRows) {
                db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
            }

            db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (0, ?1)").Bind(1, initialState.dump()).Run();
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
    if (!std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' does not exist", file.generic_string()));
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
            throw StoreError(std::format("'{}': run_meta has no '{}'", file.generic_string(), key));
        }
        return it->second;
    };

    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     file.generic_string(), value("schema_version"), SCHEMA_VERSION));
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
                                     file.generic_string(), seedText));
    }

    try {
        meta.settings = nlohmann::json::parse(value("settings"));
    } catch (const nlohmann::json::parse_error& error) {
        throw StoreError(std::format("'{}': run_meta 'settings' is not valid JSON: {}",
                                     file.generic_string(), error.what()));
    }
    meta.createdAt = value("created_at");
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
                                     _file.generic_string(), turn, error.what()));
    }
}

void RunStore::CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter)
{
    const int expected = LatestTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitTurn: expected turn {}, got {}", expected, record.number));

    // The turn, its resulting state and its notes land together or not at all.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO turns (number, input, resolve, rolls, applied, rejected, narration, timings) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)")
            .Bind(1, record.number)
            .Bind(2, record.input)
            .Bind(3, record.resolve.dump())
            .Bind(4, record.rolls.dump())
            .Bind(5, record.applied.dump())
            .Bind(6, record.rejected.dump())
            .Bind(7, record.narration)
            .Bind(8, record.timings.dump())
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

std::optional<TurnRecord> RunStore::Turn(int number)
{
    std::vector<TurnRecord> found = Turns(number, number);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::move(found.front());
}

std::vector<TurnRecord> RunStore::Turns(int fromTurn, int toTurn)
{
    Statement query = _db.Prepare("SELECT number, input, resolve, rolls, applied, rejected, narration, timings "
                                  "FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
    query.Bind(1, fromTurn).Bind(2, toTurn);

    std::vector<TurnRecord> records;
    while (query.Step()) {
        TurnRecord record;
        record.number = static_cast<int>(query.ColumnInt(0));

        // Corrupt saved data is a StoreError naming the file, turn and column, not a raw parse exception.
        auto parseColumn = [&](int column, const char* name) {
            try {
                return nlohmann::json::parse(query.ColumnText(column));
            } catch (const nlohmann::json::parse_error& error) {
                throw StoreError(std::format("'{}': turn {} column '{}' is not valid JSON: {}",
                                             _file.generic_string(), record.number, name, error.what()));
            }
        };

        record.input = query.ColumnText(1);
        record.resolve = parseColumn(2, "resolve");
        record.rolls = parseColumn(3, "rolls");
        record.applied = parseColumn(4, "applied");
        record.rejected = parseColumn(5, "rejected");
        record.narration = query.ColumnText(6);
        record.timings = parseColumn(7, "timings");
        records.push_back(std::move(record));
    }

    // Attach each turn's notes, in the order they were written. Turns are ascending, so index them by number.
    std::map<int, TurnRecord*> byNumber;
    for (TurnRecord& record : records) {
        byNumber.emplace(record.number, &record);
    }
    Statement noteQuery = _db.Prepare("SELECT turn, text FROM notes WHERE turn BETWEEN ?1 AND ?2 ORDER BY id");
    noteQuery.Bind(1, fromTurn).Bind(2, toTurn);
    while (noteQuery.Step()) {
        // Notes commit with their turn, so an orphan note means the saved data was damaged.
        const int noteTurn = static_cast<int>(noteQuery.ColumnInt(0));
        const auto it = byNumber.find(noteTurn);
        if (it == byNumber.end()) {
            throw StoreError(std::format("'{}': note for turn {} has no matching turn row", _file.generic_string(), noteTurn));
        }
        it->second->notes.push_back(noteQuery.ColumnText(1));
    }
    return records;
}

void RunStore::TruncateAfter(int turn)
{
    MORPHEUS_REQUIRE(turn >= 0, "TruncateAfter: turn must be >= 0");

    // Everything that happened after `turn` goes, in one transaction. Summaries that end at or before `turn`
    // only cover unchanged turns, so they stay.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM turns WHERE number > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM state_snapshots WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM summaries WHERE to_turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM notes WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM embeddings WHERE kind = 'turn' AND CAST(ref_id AS INTEGER) > ?1").Bind(1, turn).Run();
    });
}

void RunStore::AddSummary(const Summary& summary)
{
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("AddSummary: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
        .Bind(1, summary.fromTurn)
        .Bind(2, summary.toTurn)
        .Bind(3, summary.text)
        .Run();
}

std::vector<Summary> RunStore::Summaries()
{
    Statement query = _db.Prepare("SELECT from_turn, to_turn, text FROM summaries ORDER BY from_turn, id");
    std::vector<Summary> summaries;
    while (query.Step()) {
        summaries.push_back({static_cast<int>(query.ColumnInt(0)), static_cast<int>(query.ColumnInt(1)),
                             query.ColumnText(2)});
    }
    return summaries;
}

std::vector<Note> RunStore::Notes()
{
    Statement query = _db.Prepare("SELECT turn, text FROM notes ORDER BY turn, id");
    std::vector<Note> notes;
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
        "SELECT ref_id, vector, content_hash FROM embeddings WHERE kind = ?1 AND model = ?2 ORDER BY ref_id");
    query.Bind(1, KindName(kind)).Bind(2, model);

    std::vector<EmbeddingRow> rows;
    while (query.Step()) {
        rows.push_back({kind, query.ColumnText(0), model, query.ColumnFloats(1), query.ColumnText(2)});
    }
    return rows;
}

RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)
    : _db(std::move(db))
    , _meta(std::move(meta))
    , _file(std::move(file))
{
}

} // namespace morpheus::store
