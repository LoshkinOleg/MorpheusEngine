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
    // Throws ContractError if record.number is not LatestTurn()+1, and on a hosted run.
    void CommitTurn(const core::TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist. record.notes holds that turn's notes in the order they were written.
    // Throws StoreError if a stored column is not valid JSON or has the wrong shape, or a note has no matching
    // turn row.
    std::optional<core::TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending, each with its notes. Throws StoreError if a stored column is not
    // valid JSON or has the wrong shape, or a note has no matching turn row.
    std::vector<core::TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn and note embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0, and on a hosted run.
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
    // ContractError for an unknown conversation, unless 1 <= fromTurn <= toTurn, and unless toTurn is past the
    // conversation's folded_through and at most LatestHostedTurn().
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

    // Throws ContractError naming `method` unless this is an owned run (CommitTurn and TruncateAfter).
    void RequireOwned(const char* method) const;

    // Throws ContractError naming `method` unless this is a hosted run.
    void RequireHosted(const char* method) const;
};

} // namespace morpheus::store
