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

enum class EmbeddingKind { Lore, Turn, Note, Summary };

// refId: lore subject for Lore, decimal turn number for Turn, "<turn>.<index>" for Note (index: the note's
// position among that turn's notes), decimal summary id for Summary.
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
// conversations, hosted_turns, conversation_events, actor_memory, pending_shares. Every file has every table;
// summaries and embeddings serve both kinds, and turn numbers run across the whole run. An owned run's summaries have
// no actor; each of a hosted run's belongs to one NPC (per-speaker memory spec).
class RunStore {
public:
    // 5: the summaries' per-actor columns, actor_memory and pending_shares. 4: hosted_turns.events_omitted. 3:
    // run_meta.kind and the hosted tables. 2: turns.digest (it also covers turns.attempt). A version 2, 3 or 4 run is
    // upgraded in place on open; any other version is refused.
    static constexpr int SCHEMA_VERSION = 5;

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

    // Opens an existing run. An older file is first upgraded in place to version 5, in one transaction: a version 2
    // file gains the hosted tables and is marked owned, a version 3 file gains hosted_turns.events_omitted, and each
    // of them gains version 5's per-actor summary columns and tables. A version 4 hosted run's run-wide summaries are
    // then moved to the NPCs who witnessed a turn in their range (each NPC witness's mark in a conversation starting at
    // its folded_through); an owned run's data is not touched. Throws StoreError if it is missing or was written with
    // another schema version, its meta (rng_seed, settings, kind) is damaged, or a hosted turn the move reads is.
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

    // The owned game's run-wide summaries, those without an actor. AddSummary, ReplaceSummaries and Summaries throw
    // ContractError on a hosted run.

    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const core::Summary& summary);

    // Condensing: replaces every run-wide summary with `merged` in one transaction. Throws ContractError unless
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

    // A fold by the speaker: adds `summary` (its actor's, of conversation `id`) and moves the conversation's
    // folded_through and the actor's remembered-through mark in it to summary.toTurn, in one transaction. Returns the
    // new summary's id. Throws ContractError for an unknown conversation, an empty summary.actorId, a
    // summary.conversationId other than `id`, unless 1 <= fromTurn <= toTurn, and unless toTurn is past the
    // conversation's folded_through and the actor's mark and at most LatestHostedTurn().
    std::int64_t FoldConversation(std::string_view id, const core::Summary& summary);

    // Moves conversation `id`'s folded_through to `turn` without a summary: an ended conversation folded in the
    // background. Throws ContractError for an unknown conversation, and unless `turn` is past its folded_through and
    // at most LatestHostedTurn().
    void MarkFolded(std::string_view id, int turn);

    // One NPC's summaries, by fromTurn then id: the shown ones, or the superseded ones too. Throws ContractError for an
    // empty actorId.
    std::vector<core::Summary> ActorSummaries(std::string_view actorId, bool includeSuperseded);

    // Every summary of the run, by fromTurn then id. Throws StoreError naming the file and actor_id for a row without
    // its actor.
    std::vector<core::Summary> HostedSummaries();

    // Adds `summary` (every field but id; a shared copy names its teller and the summary it copies) and returns its
    // id. Throws ContractError for an empty actorId and unless 1 <= fromTurn <= toTurn.
    std::int64_t AddActorSummary(const core::Summary& summary);

    // A catch-up fold: adds `summary` and moves its actor's remembered-through mark in its conversation to
    // summary.toTurn, in one transaction; the conversation's folded_through stays. Returns the summary's id. Throws
    // ContractError like AddActorSummary and SetRememberedThrough, and for an empty conversationId.
    std::int64_t RememberFold(const core::Summary& summary);

    // Condensing one NPC's memory: marks every shown summary of `actorId` superseded and adds `condensed`, in one
    // transaction. Returns the condensed summary's id. Throws ContractError unless condensed.actorId is `actorId`,
    // its conversationId and toldBy are empty, and 1 <= fromTurn <= toTurn.
    std::int64_t CondenseActorSummaries(std::string_view actorId, const core::Summary& condensed);

    // How far `actorId` remembers conversation `conversationId`: its turns up to this number are in the actor's
    // summaries. 0 when there is no mark.
    int RememberedThrough(std::string_view conversationId, std::string_view actorId);

    // Moves `actorId`'s mark in conversation `conversationId` to `turn`. Throws ContractError for an unknown
    // conversation, an empty actorId, and unless `turn` is past the current mark.
    void SetRememberedThrough(std::string_view conversationId, std::string_view actorId, int turn);

    // Records a share that waits until the teller's memory of the conversation is complete, and returns its id
    // (share.id is ignored). Throws ContractError for an empty actor or conversation.
    std::int64_t AddPendingShare(const core::PendingShare& share);

    // Every pending share, in the order they were added.
    std::vector<core::PendingShare> PendingShares();

    // Removes pending share `id`.
    void RemovePendingShare(std::int64_t id);

    // Appends `events` to conversation `id`, in order, each with `afterTurn` (the conversation's latest turn, 0 before
    // its first), in one transaction. Throws ContractError for an unknown conversation.
    void AddConversationEvents(std::string_view id, int afterTurn, const std::vector<nlohmann::json>& events);

    // Conversation `id`'s events in the order they were recorded. Throws StoreError when one is not valid JSON or not
    // an object with a string `text`.
    std::vector<core::ConversationEvent> ConversationEvents(std::string_view id);

    // Highest committed hosted turn number, 0 before the first.
    int LatestHostedTurn();

    // Appends hosted turn LatestHostedTurn()+1. Throws ContractError if record.number is not that, or its
    // conversation is unknown.
    void CommitHostedTurn(const core::HostedTurn& record);

    // Conversation `conversationId`'s turns, ascending. Throws StoreError naming the file, the turn and the column if a
    // stored column is not valid JSON or has the wrong shape, including an observation without the actors core
    // (core::ReadActors, the rules HostedWorld::Observe applies; each fault named by its pointer) or a speaker that is
    // not one of its actors.
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

    // The summary in the current row of `query`, selected as id, from_turn, to_turn, text, actor_id, conversation_id,
    // told_by, source_id, superseded. A hosted row without its actor is damaged saved data.
    core::Summary ReadSummaryRow(Statement& query) const;

    // Moves conversation `id`'s folded_through to `turn` (FoldConversation and MarkFolded, named by `method`). Throws
    // ContractError for an unknown conversation, and unless `turn` is past its folded_through and at most
    // LatestHostedTurn().
    void MoveFoldedThrough(const char* method, std::string_view id, int turn);
};

} // namespace morpheus::store
