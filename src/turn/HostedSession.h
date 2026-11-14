#pragma once

#include <cstdint>
#include <filesystem>
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
#include "turn/Retrieval.h"

namespace morpheus::turn {

// Every hosted run's world id (store::RunMeta::worldId).
constexpr std::string_view HOSTED_WORLD_ID = "hosted";

// One open hosted run (hosted-world spec, section 1): the counterpart of turn::Session for a world the host owns and
// sends as data. Every backend call happens on the caller's thread. Not thread-safe.
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
    // lore; or reopens it: the same host data (equal content hash) reopens as it is, changed host data (a mod update)
    // replaces the stored copy, and changed lore is re-embedded. The host data is checked first (HostedWorld's
    // DataError). Throws DataError for a file that holds an owned run, StoreError for a damaged one, and DataError if
    // lore cannot be embedded (a new file is then removed).
    static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                              std::uint64_t seed, inference::LlmBackend& backend);

    // Starts a conversation of `participantIds` (in this join order) and returns its id. Every participant must be an
    // actor of the observation, once; their names and bios are taken from it and stored. Throws DataError naming the
    // field ("participants", "participants/<i>", or the observation's pointer).
    std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation);

    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn. Throws DataError naming the field.
    void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events);

    // Removes those who `left`, then adds those who `joined` at the end of the join order, with their names and bios
    // from the observation. A conversation keeps at least one participant. Throws DataError naming the field
    // ("left/<i>", "joined/<i>", "left", or the observation's pointer).
    void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                         const std::vector<std::string>& left, const nlohmann::json& observation);

    // Marks the conversation ended; the next Maintain folds its remaining turns. Throws DataError for an unknown or
    // ended conversation.
    void EndConversation(std::string_view conversationId);

    const store::RunMeta& Meta() const;
    std::optional<core::Conversation> Conversation(std::string_view id);
    std::vector<core::HostedTurn> Turns(std::string_view conversationId);         // Oldest first.
    std::vector<core::ConversationEvent> Events(std::string_view conversationId); // Recorded between turns, in order.
    std::vector<core::Summary> Summaries();
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the inspector and the tests.

private:
    HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                  inference::LlmBackend& backend);

    // Conversation `conversationId`, which must exist and be open. Throws DataError "conversation_id: ..." otherwise.
    core::Conversation OpenConversation(std::string_view conversationId);

    // A conversation's turns as transcript entries, oldest first: the folded ones (folded == true) or those still in
    // L3. A turn's user message is the events before it (recorded since the conversation's previous turn, then its
    // request's own) in parentheses and the player's line, one per line; HOSTED_TURN_CUE when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>", without the params when there are none and
    // without the bracket for a refused action. Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, bool folded);

    // Indexes every conversation's folded turns as searchable history.
    void SyncHistory();
};

} // namespace morpheus::turn
