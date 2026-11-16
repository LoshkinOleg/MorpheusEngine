#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// Every hosted run's world id (store::RunMeta::worldId).
constexpr std::string_view HOSTED_WORLD_ID = "hosted";

// One hosted turn as the host asks for it (hosted-world spec, section 1).
struct HostedTurnRequest {
    std::string conversationId = {};
    std::string speakerId = {};
    std::optional<std::string> playerText = {}; // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};    // Objects with a `text` string, added to the conversation before the turn.
    nlohmann::json observation = nlohmann::json::object();
    std::optional<std::vector<std::string>> allowedActions = {}; // Action ids; absent means every action of the catalog.
    bool review = false; // Review the line before it is delivered (hosted-review spec); off: as before.
};

// What a hosted turn reports while it runs.
struct HostedCallbacks {
    // Once the speaker is known, before the resolve call: the named speaker, or the NPC a "decide" turn chose.
    std::function<void(const std::string&)> onSpeaker = {};
    // After the resolve call, before the line starts: what became of the speaker's proposed action.
    std::function<void(const core::Adjudication&)> onAction = {};
    // The line's pieces as they are written.
    inference::TokenCallback onToken = {};
};

// What a share did: how many summaries it copied now, and whether it waits for the teller's memory (per-speaker
// memory spec, section 2).
struct ShareResult {
    int shared = 0;
    bool pending = false;
};

// What a rewind (HostedSession::Rewind) changed, net: whether the open rollback changed, the turns it hid and the turns
// it showed again (a turn shown by the undo and hidden again by the new rollback counts as neither), and the game time
// of the rollback open after it (none when it left none open).
struct RewindResult {
    bool rewound = false;
    int hiddenTurns = 0;
    int restoredTurns = 0;
    std::optional<double> rollbackTime = {};
};

// One open hosted run (hosted-world spec, section 1): the counterpart of turn::Session for a world the host owns and
// sends as data. Every backend call happens on the caller's thread.
//
// D1's contract: one HostedSession per run file; two sessions on one file are not supported. It is not thread-safe,
// so its caller serializes every call on a run, turns of different conversations included; Maintain runs between
// them and stops through its stop token, or returns between its steps through its yield token, when the next call
// is due. Every model call uses the backend's default slot (GenerationSpec::slot is left at -1). Choosing a slot per
// conversation and any concurrency across conversations are left to D2 (morpheus_server), which can add them
// without changing this API.
class HostedSession {
private:
    HostedWorld _world; // Set by the constructor.
    core::Settings _settings = {};
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index; // Set by the constructor.
    context::Assembly _lastAssembly = {};

public:
    // Creates the run in `file` when the file does not exist, with `seed` for sampling, and indexes the retrievable
    // lore; or reopens it: the same host data (equal content hash) reopens as it is; with changed host data (a mod
    // update) the changed lore is re-embedded first, then the new host data replaces the stored copy. The host data
    // is checked first (HostedWorld's DataError), then the settings: a summary_cap not below tail_allowance is a
    // DataError naming both (the speaker's memories sit in the tail). Then the system text (the run's pack's) is
    // counted with `backend`: over engine_rules_cap is a DataError "prompts/system: <n> tokens, over
    // engine_rules_cap <cap>". Throws DataError for a file that holds an owned run, StoreError for a damaged one,
    // DataError if the backend refuses a lore entry (too long to embed), and InferenceError if the backend cannot be
    // reached or fails while counting the system text (before the file is touched) or while embedding: either way
    // a new file is then removed, and a reopened run keeps its stored host data and hash.
    static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                              std::uint64_t seed, inference::LlmBackend& backend);

    // Every writer of a new record below first abandons an open rollback, once its checks passed and just before it
    // records: its hidden records stay hidden for good (Rewind). StartConversation, SetParticipants and
    // EndConversation always record; RecordEvents when it has an event; RunTurn just before it commits its turn (the
    // catch-up's and the fit loop's folds before that are written in the rollback, as Maintain's are); ShareMemories
    // when it stores a pending share or makes a copy. A refused request, a turn cancelled or failed, an empty events
    // list and a share already pending or already copied record nothing and leave the rollback open.

    // Starts a conversation of `participantIds` (in this join order) and returns its id. Every participant must be an
    // actor of the observation, once; their names and bios are taken from it and stored. Throws DataError naming the
    // field ("participants", "participants/<i>", or the observation's pointer).
    std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation);

    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn: every event recorded since its last turn joins that turn's tail. When the tail is over
    // tail_allowance, the oldest events (these first, then the request's own) are left out of that turn's prompt
    // (RunTurn); the store keeps them all. An empty list records nothing. Throws DataError naming the field.
    void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events);

    // Removes those who `left`, then adds those who `joined` at the end of the join order, with their names and bios
    // from the observation. A conversation keeps at least one participant. Throws DataError naming the field
    // ("left/<i>", "joined/<i>", "left", or the observation's pointer).
    void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                         const std::vector<std::string>& left, const nlohmann::json& observation);

    // Plays one turn (hosted-world spec, section 2): checks the request, catches the speaker's memory of the
    // conversation up to its transcript's cut (CatchUp), retrieves from lore and the run's folded turns, resolves
    // under the proposal grammar (the fit loop folding the conversation's oldest turns when L3 is over its budget),
    // adjudicates and reports the action through onAction, speaks on a prompt that extends the resolve prompt
    // (streamed through onToken), and commits the record. Player text is cleaned as Session cleans input
    // (CleanInput); nothing left means none. Returns the committed turn, or nullopt when `stop` cancelled it:
    // then no turn is committed and the request's events are not kept (folds the catch-up and the fit loop finished
    // before the stop are, as in Session), and an open rollback stays open, as it does when the turn throws. onAction
    // fires before the line starts, so a turn cancelled during the speak call has already reported its action through
    // onAction and is still not committed: the host must not take a reported action as a committed turn until RunTurn
    // returns one. Throws DataError naming the field for a malformed request (before any model call), TurnError when
    // the observation, the player text and the instruction alone do not fit tail_allowance (before the resolve call;
    // folds the catch-up and the fit loop finished before it stay), and InferenceError from the backend. The tail holds
    // the observation, every event recorded since the conversation's last turn (RecordEvents), the request's events
    // and the player text: when it is over tail_allowance with no retrieved snippet left to shed, the oldest events
    // leave the prompt one by one until it fits. The record's eventsOmitted counts them; the store keeps them, and the
    // turn's L3 entry leaves them out as its prompt did. Model mistakes are not exceptions: a refused action is
    // recorded with its reasons and the turn goes on with no action. With `speakerId` DECIDE_SPEAKER the engine chooses
    // the speaker among the conversation's NPCs present (decide-speaker spec): the last speaker is left out on a turn
    // with no player text and no events while another remains; one candidate is chosen without a call, more with a
    // decide call; a refused decide answer goes to the candidate who spoke least recently, its reasons in
    // `timings.decide.refused`. A DataError "speaker: "decide" needs an NPC participant in the observation" when there
    // is none. The decide prompt fits as the resolve prompt does, without folds (the oldest new events, then the oldest
    // lines, leave it); when it still does not, a TurnError names the tail (over tail_allowance) or the context, before
    // any record. onSpeaker fires before the catch-up, so a turn cancelled or failing after it has reported its speaker
    // without committing, as for onAction. With `review` the line is held back, reviewed, spoken once more with the
    // review's problems when it finds any, then delivered to onToken in one piece; `timings.review` and
    // `timings.respeak` record the two calls.
    std::optional<core::HostedTurn> RunTurn(const HostedTurnRequest& request, const HostedCallbacks& callbacks,
                                            std::stop_token stop);

    // Marks the conversation ended; the next Maintain folds its remaining turns. Throws DataError for an unknown or
    // ended conversation.
    void EndConversation(std::string_view conversationId);

    // Between-turn upkeep, through the shared memory upkeep: every NPC catches up on what it witnessed in every
    // conversation. An ended conversation is first folded through its last turn (its turns become searchable history),
    // then each NPC witness's memory of it is written (in one call when it fits); in an open conversation each NPC
    // witness's memory reaches the transcript's cut. An NPC's memories over summary_cap are condensed on the way.
    // Then every pending share (ShareMemories) whose teller's memory is now complete is applied and removed.
    // `stop` cancels it, a model call in progress included. `yield` asks it to return between steps (an ended
    // conversation's mark, one NPC's catch-up in one conversation, one applied share) after it finished at least one,
    // so a caller with other work waits for one step instead of the whole pass. Returns false when stopped or when it
    // yielded; it resumes from the marks when run again. Throws DataError when summary_cap is too small for even a
    // condensed summary, and StoreError for a pending share whose conversation is missing or has no turn (damaged
    // saved data).
    bool Maintain(std::stop_token stop, std::stop_token yield = {});

    // `toActorId` learns what `fromActorId` remembers of conversation `conversationId`: copies of `fromActorId`'s own
    // summaries of it (superseded or not, shared copies excluded), each told by `fromActorId`, oldest first. A copy is
    // a snapshot, and a summary already copied to `toActorId` is not copied again. While the conversation is open or
    // `fromActorId` is behind in it, the share waits (stored once, however often it is asked for) and Maintain applies
    // it once the conversation ended and `fromActorId` caught up. Throws DataError "conversation_id: ..." for an
    // unknown conversation, "from: ..." when `fromActorId` witnessed none of its turns as an NPC, and "to: ..." for an
    // empty `toActorId`, the teller itself, or the player of any of its turns.
    ShareResult ShareMemories(const std::string& fromActorId, const std::string& toActorId,
                              const std::string& conversationId);

    // The run as it was at game time `gameTime` (rollback spec, section 3): the host calls it on every save load. It
    // undoes the open rollback, if any, then applies one to `gameTime` when that is before the latest live clock; a
    // rewind to the open rollback's own time changes nothing, and a rewind that changes nothing (rewound false) does
    // not resync the retrieval index. Hidden records are kept; a later rewind restores them
    // until a new record abandons them (a turn committed, events recorded, a conversation started or ended,
    // participants changed, a share stored or copied: see the writers above). The background upkeep then refolds what
    // NPCs are missing. Throws DataError "game_time: ..." when the host data names no clock or `gameTime` is not
    // finite, and InferenceError when the backend fails while the retrieval index is synced afterwards (an embedding):
    // the store has already rewound then, and the next full sync (SyncHistory: a later rewind that changes what is
    // hidden, a fold during a turn, Maintain's mark of an ended conversation, or reopening the run) repairs the index.
    RewindResult Rewind(double gameTime);

    const store::RunMeta& Meta() const;
    std::optional<core::Conversation> Conversation(std::string_view id);
    std::vector<core::HostedTurn> Turns(std::string_view conversationId);         // Oldest first.
    std::vector<core::ConversationEvent> Events(std::string_view conversationId); // Recorded between turns, in order.
    std::vector<core::Summary> Memories(std::string_view actorId); // Every summary of the actor, superseded included.
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the inspector and the tests.

private:
    HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                  inference::LlmBackend& backend);

    // Conversation `conversationId`, which must exist and be open. Throws DataError "conversation_id: ..." otherwise.
    core::Conversation OpenConversation(std::string_view conversationId);

    // A new record while a rollback is open: the hidden future is abandoned first (rollback spec, "Playing on"). Every
    // writer of a new record calls it after its checks, just before it writes that record (RunTurn just before its
    // commit); Maintain never does.
    void AbandonRollbackForNewRecord();

    // Of `turns` (the conversation's turns as the store holds them, oldest first), those with afterTurn < number <=
    // throughTurn as transcript entries, oldest first, only those `witnessId` witnessed (its speaker or a listener)
    // when it is not empty. Rendering as before: a turn's user
    // message is the events before it (recorded since the conversation's previous turn, then its request's own, less
    // the ones its prompt left out) in parentheses and the player's line; the pack's turn cue when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>". Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation,
                                                  const std::vector<core::HostedTurn>& turns, int afterTurn,
                                                  int throughTurn, std::string_view witnessId);

    // Indexes every conversation's folded turns as searchable history, then the memories (SyncMemories). Needed only
    // when a conversation's folded_through moved: Open, a fold during a turn, Maintain's mark of an ended one.
    void SyncHistory();

    // Indexes every NPC summary of the run (shown, superseded and shared) as a searchable memory of its NPC; alone
    // after a write that changes only summaries (a catch-up fold, a condense, a share).
    void SyncMemories();

    // One pass of the memory upkeep for `actorId` in conversation `conversationId`: the hosted prompt with
    // `transcript` as L3, `actorId`'s shown summaries as its memories (tellers by name), `hits` and the tail parts
    // `inputs` already holds, read from the store as it is now.
    FitPass Pass(std::string_view conversationId, std::string_view actorId,
                 std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits,
                 context::HostedInputs inputs);

    // The shared memory upkeep for `actorId`'s memory of conversation `conversationId`, its seeds naming turn `turn`:
    // `actorId`'s scope with the hosted texts, which name the actor and the tellers as Pass does (the conversation's
    // participants, then the actors of `observation`, then the id). A fold's summary is the actor's; with
    // foldsConversation it also moves the conversation's folded mark (a fold during the actor's turn), otherwise only
    // the actor's own (a catch-up).
    MemoryUpkeep Upkeep(const std::string& conversationId, const std::string& actorId,
                        const nlohmann::json& observation, int turn, bool foldsConversation);

    // Catches `actorId` up on conversation `conversationId` (RunTurn, before the actor speaks, and Maintain): the turns
    // it witnessed after its remembered-through mark through `throughTurn` fold into its memory, the memory calls'
    // seeds naming turn `seedTurn`; then its mark is `throughTurn`, the turns it did not witness included. False when
    // `stop` cancelled it (what it folded before stays).
    bool CatchUp(const std::string& conversationId, const std::string& actorId, int throughTurn, int seedTurn,
                 std::stop_token stop);

    // Copies to `toActorId` the summaries of `fromActorId`'s own memory of conversation `conversationId` it does not
    // have yet (ShareMemories, and Maintain for a pending share), and returns how many. With abandonsRollback (a share
    // the host asked for), an open rollback is abandoned before the first copy; with none to copy it stays open.
    int ApplyShare(const std::string& fromActorId, const std::string& toActorId, const std::string& conversationId,
                   bool abandonsRollback);

    // Every NPC who spoke or listened in `turns`, in first-seen order. A listener who is the player in its turn's
    // observation is left out.
    std::vector<std::string> NpcWitnesses(const std::vector<core::HostedTurn>& turns);
};

} // namespace morpheus::turn
