#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/TurnRecord.h"
#include "store/Sqlite.h"

namespace morpheus::store {

// Turn data types live in core; store re-exports them so callers can keep writing store::TurnRecord.
using core::Note;
using core::Summary;
using core::TurnRecord;

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
};

enum class EmbeddingKind { Lore, Turn };

// refId: lore subject for Lore, decimal turn number for Turn.
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
    std::string contentHash = {}; // Hash of the exact text that was embedded. Callers re-embed when it differs
                                  // from the hash of the current text, so edited lore never keeps a stale vector.
};

// One run's SQLite database (saves/<world>/<run>.db). The only writer of run data.
// Tables: run_meta, turns, state_snapshots, summaries, notes, embeddings.
class RunStore {
public:
    static constexpr int SCHEMA_VERSION = 1;

private:
    Database _db;
    RunMeta _meta = {};
    std::filesystem::path _file = {}; // Only used to name the file in error messages.

public:
    // Creates a new run database holding the meta and the turn-0 snapshot. Throws StoreError if the file exists.
    // If creation fails part way, the partial file is removed.
    static RunStore Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState);

    // Opens an existing run. Throws StoreError if it is missing or was written with another
    // schema version, or its meta (rng_seed, settings) is damaged.
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
    void CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist. record.notes holds that turn's notes in the order they were written.
    // Throws StoreError if a stored column is not valid JSON and StoreError if a note has no matching turn row.
    std::optional<TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending, each with its notes. Throws StoreError if a stored column is not
    // valid JSON and StoreError if a note has no matching turn row.
    std::vector<TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);

    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const Summary& summary);

    // Ordered by fromTurn.
    std::vector<Summary> Summaries();

    // Every committed note, ordered by turn, then the order written. For context assembly in assisted mode.
    std::vector<Note> Notes();

    // Inserts or replaces the vector and content hash for (kind, refId, model).
    // Throws ContractError on an empty vector or an empty contentHash.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model, with the content hash it was stored with.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);

private:
    RunStore(Database db, RunMeta meta, std::filesystem::path file);
};

} // namespace morpheus::store
