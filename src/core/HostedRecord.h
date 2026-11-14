#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// One participant of a hosted conversation, as it joined: the actor's id, with its name and bio from the observation
// it joined with. The participants layer of a hosted prompt shows them in join order.
struct Participant {
    std::string id = {};
    std::string name = {};
    std::string bio = {}; // Empty when the observation gave none.
};

// One conversation of a hosted run (hosted-world spec, section 5).
struct Conversation {
    std::string id = {};
    std::vector<Participant> participants = {}; // In join order.
    bool ended = false;
    int foldedThrough = 0; // This conversation's turns up to this number are folded into summaries; 0 when none are.
};

// An event recorded between turns (turn::HostedSession::RecordEvents): the host's JSON object, with its `text`.
struct ConversationEvent {
    int afterTurn = 0; // The conversation's latest turn when it was recorded; 0 before its first.
    nlohmann::json event = nlohmann::json::object();
};

// What became of the speaker's proposed action (turn::HostedWorld::Adjudicate). A requested action is the host's to
// carry out; its outcome comes back later as an event. A refused one has the parse or validation errors as reasons,
// and the speaker takes no action.
struct Adjudication {
    bool requested = false;
    std::string actionId = {};
    nlohmann::json params = nlohmann::json::object();
    std::vector<std::string> reasons = {};
};

// One committed hosted turn (hosted-world spec, section 2, step 4). Stored by store::RunStore; rendered into a hosted
// prompt's L3 by turn::HostedSession.
struct HostedTurn {
    int number = 0; // Run-wide, shared with summaries' turn ranges.
    std::string conversationId = {};
    std::string speakerId = {};
    std::vector<std::string> listenerIds = {};       // The other participants present, in join order.
    std::optional<std::string> playerText = {};      // Absent for a turn between NPCs.
    std::vector<nlohmann::json> events = {};         // The events this turn's request added, in order.
    nlohmann::json observation = nlohmann::json::object();
    std::string resolveOutput = {};                  // The resolve call's text exactly as the model wrote it.
    Adjudication adjudication = {};
    std::string line = {};                           // The spoken line, trimmed.
    nlohmann::json timings = nlohmann::json::object();
};

} // namespace morpheus::core
