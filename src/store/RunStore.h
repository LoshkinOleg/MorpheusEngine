#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "store/Sqlite.h"

namespace morpheus::store {

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
};

// Everything one committed turn produced, as stored in the turns table. JSON members hold the turn module's
// structured outputs verbatim.
struct TurnRecord {
    int number = 0;
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output.
    nlohmann::json rolls = nlohmann::json::array();    // Check results.
    nlohmann::json applied = nlohmann::json::array();  // Applied changes.
    nlohmann::json rejected = nlohmann::json::array(); // Rejected changes with reasons.
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
};

// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
};

// An append-only memory written by the model in assisted memory mode.
struct Note {
    int turn = 0;
    std::string text = {};
};

enum class EmbeddingKind { Lore, Turn };

// refId: lore subject for Lore, decimal turn number for Turn.
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
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

    // Appends turn LatestTurn()+1 and the state it produced, in one transaction.
    // Throws ContractError if record.number is not LatestTurn()+1.
    void CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist. Throws StoreError if a stored column is not valid JSON.
    std::optional<TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending. Throws StoreError if a stored column is not valid JSON.
    std::vector<TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);

    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const Summary& summary);

    // Ordered by fromTurn.
    std::vector<Summary> Summaries();

    void AddNote(const Note& note);

    // Ordered by turn, then insertion.
    std::vector<Note> Notes();

    // Inserts or replaces the vector for (kind, refId, model). Throws ContractError on an empty vector.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);

private:
    RunStore(Database db, RunMeta meta, std::filesystem::path file);
};

} // namespace morpheus::store
