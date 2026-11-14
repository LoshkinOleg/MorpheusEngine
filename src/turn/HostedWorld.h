#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/World.h"

namespace morpheus::turn {

// What the host sends once per run (hosted-world spec, section 1): its action catalog and the JSON Schema of its
// observation, both as the host wrote them, and its lore.
struct HostData {
    nlohmann::json catalog = nlohmann::json::object();
    nlohmann::json observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> lore = {};
};

// HostData as a hosted run stores it and as the fixtures write it:
// {"catalog": ..., "observation_schema": ..., "lore": [{"subject": ..., "data": ..., "always_on": ...}]}.
nlohmann::json HostDataToJson(const HostData& data);

// Reads HostDataToJson's form; "lore" may be absent (no lore) and "always_on" defaults to false. Checks the shape only
// (HostedWorld checks the content): throws DataError "host data at <pointer>: <problem>".
HostData HostDataFromJson(const nlohmann::json& json);

// The content hash of `data` (core::ContentHash over HostDataToJson's text) that a hosted run stores: changed host
// data, such as a mod update, has another.
std::string HostDataHash(const HostData& data);

// One actor of an observation, as the engine reads it.
struct Actor {
    std::string id = {};
    std::string name = {};
    bool isPlayer = false;
    std::string bio = {}; // Empty when the observation gives none.
};

// An observation that passed HostedWorld::Observe: the host's JSON as sent, and its actors in their order.
struct Observation {
    nlohmann::json json = nlohmann::json::object();
    std::vector<Actor> actors = {};

    // The actor with `id`, or nullptr when it is not in the observation.
    const Actor* Find(std::string_view id) const;
    // The one actor that is the player. Throws ContractError for an observation without one (Observe refuses those).
    const Actor& Player() const;
};

// A conversation's participants, in join order.
using Participants = std::vector<core::Participant>;

// R5's three operations over the host data of one hosted run (hosted-world spec, section 1): Observe,
// ProposalGrammar and Adjudicate. It never simulates the world: an accepted action is requested, not applied.
class HostedWorld {
private:
    enum class ParamType { Actor, String, Integer, Enum };
    enum class Scope { Present, PresentNonPlayer, Participant };

    // One parameter of a catalog action.
    struct Param {
        std::string name = {};
        ParamType type = ParamType::String;
        Scope scope = Scope::Present;                    // Actor: whose ids a turn offers.
        nlohmann::json schema = nlohmann::json::object(); // String, Integer, Enum: the parameter's proposal schema.
    };

    // One catalog action, its parameters by name.
    struct Action {
        std::string id = {};
        std::vector<Param> params = {};
    };

    std::vector<Action> _actions = {}; // In catalog order.
    nlohmann::json _observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> _lore = {};

public:
    // Checks the host data and keeps it. Throws DataError naming the first fault: "catalog at <pointer>: ...",
    // "observation_schema: schema at <pointer>: ..." (the schema must be in grammar::Validate's extended subset) or
    // "lore at /<index>/<field>: ..." (subjects non-empty and unique, data non-empty).
    explicit HostedWorld(const HostData& data);

    const std::vector<core::LoreEntry>& Lore() const;

    // Every action id of the catalog, in order: what a turn allows when the host names no allowed actions.
    std::vector<std::string> ActionIds() const;

    // Validates `observation` and returns it with its parsed core. First the core, which the engine needs whatever the
    // host's schema says: `actors`, an array of objects each with `id` and `name` (non-empty strings), `is_player`
    // (a boolean) and an optional `bio` (a string); ids unique; exactly one player. Then grammar::Validate against the
    // host's schema. Every fault of both is collected; any fault is a DataError
    // "observation at <pointer>: <problem>; observation at ...".
    Observation Observe(const nlohmann::json& observation) const;

    // The core actors of `observation`, read as Observe reads them but without the host's schema, so a stored
    // observation still reads after the host data changed. Throws Observe's DataError for a fault of the core.
    static std::vector<Actor> ActorsOf(const nlohmann::json& observation);

    // Events are JSON objects with a `text` string; their other fields are the host's. Throws DataError
    // "events/<i>: ..." or "events/<i>/text: ..." for the first that is not.
    static void CheckEvents(const std::vector<nlohmann::json>& events);

    // The JSON Schema of the speaker's possible actions this turn (hosted-world spec, section 3): a oneOf over the
    // allowed actions in catalog order, discriminated by "action", each branch {"action": <const id>, "params": {...}}
    // with every parameter required and no extra keys. An actor parameter is an enum of the actor ids its scope gives
    // (present: every actor of the observation; present_non_player: those but the player; participant: the
    // participants present), never the speaker, each once, in observation order; an action whose actor enum would be
    // empty is left out. Throws DataError "allowed_actions/<i>: ..." for an id not in the catalog or listed twice, and
    // "allowed_actions: ..." when no allowed action is left.
    nlohmann::json ProposalSchema(const Observation& observation, const Participants& participants,
                                  const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // grammar::FromSchema of ProposalSchema(...): the compact JSON the resolve call may write.
    std::string ProposalGrammar(const Observation& observation, const Participants& participants,
                                const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // What becomes of the model's proposal: parsed (a parse failure is a refusal: the grammar admits text the JSON
    // parser refuses, such as lone surrogate escapes and numbers it cannot hold, like 1e999), then grammar::Validate
    // against `proposalSchema`. A valid proposal is requested with its action id and params; otherwise it is refused,
    // and the reasons are the parse error or each validation error as "<pointer>: <message>". Never throws for the
    // model's text.
    core::Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const;
};

} // namespace morpheus::turn
