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
    kind   TEXT NOT NULL,
    ref_id TEXT NOT NULL,
    model  TEXT NOT NULL,
    vector BLOB NOT NULL,
    PRIMARY KEY (kind, ref_id, model)
);
)sql";

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

RunStore::RunStore(Database db, RunMeta meta, std::filesystem::path file)
    : _db(std::move(db))
    , _meta(std::move(meta))
    , _file(std::move(file))
{
}

} // namespace morpheus::store
