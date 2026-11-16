#pragma once

#include <cstdint>
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

// One actor of a hosted observation, as the engine reads it.
struct Actor {
    std::string id = {};
    std::string name = {};
    bool isPlayer = false;
    std::string bio = {}; // Empty when the observation gives none.
    // The actor's knowledge scopes: the scoped lore it may retrieve. Empty when the observation gives none.
    std::vector<std::string> knows = {};
};

// One fault of an observation: where it is (a JSON pointer; "" for the observation itself) and what is wrong.
struct ObservationFault {
    std::string pointer = {};
    std::string problem = {};
};

// An observation's actors core as ReadActors read it: the actors read whole, in order, and every fault found. The
// core is sound only when `faults` is empty.
struct ActorsCore {
    std::vector<Actor> actors = {};
    std::vector<ObservationFault> faults = {};
};

// Reads the fixed core of a hosted observation that the engine needs whatever the host's schema says (hosted-world
// spec, section 1): an object with `actors`, an array of objects each with `id` and `name` (non-empty strings),
// `is_player` (a boolean), an optional `bio` (a string) and an optional `knows` (an array of non-empty strings); ids unique; exactly one player. Every mistake is a fault;
// never throws. turn::HostedWorld turns the faults into its DataError, store::RunStore into its StoreError.
ActorsCore ReadActors(const nlohmann::json& observation);

// One conversation of a hosted run (hosted-world spec, section 5).
struct Conversation {
    std::string id = {};
    std::vector<Participant> participants = {}; // In join order.
    bool ended = false;
    int foldedThrough = 0; // This conversation's turns up to this number are folded into summaries; 0 when none are.
    std::optional<double> clock = {}; // The game time it started at; none without a host clock.
};

// An event recorded between turns (turn::HostedSession::RecordEvents): the host's JSON object, with its `text`.
struct ConversationEvent {
    int afterTurn = 0; // The conversation's latest turn when it was recorded; 0 before its first.
    nlohmann::json event = nlohmann::json::object();
    std::optional<double> clock = {}; // The run's latest seen clock when it was recorded; none before any.
};

// A share of one NPC's memories of a conversation with another, waiting until the teller's memory of it is complete
// (per-speaker memory spec, section 2).
struct PendingShare {
    std::int64_t id = 0;
    std::string fromActor = {};
    std::string toActor = {};
    std::string conversationId = {};
    std::optional<double> clock = {};        // The run's latest seen clock when it was created; none before any.
    std::optional<double> appliedClock = {}; // The run's latest seen clock when it was applied; none while pending.
    std::int64_t unappliedBy = 0;            // The rollback that made it pending again; 0 for none.
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
    // How many of the events before the turn (recorded since the conversation's previous turn, then the request's)
    // its prompt left out, the oldest first, because the tail was over tail_allowance. The store keeps them all.
    int eventsOmitted = 0;
    nlohmann::json timings = nlohmann::json::object();
    std::optional<double> clock = {}; // Its observation's game time; none without a host clock.
};

} // namespace morpheus::core
