#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/World.h"
#include "turn/PromptPack.h"

namespace morpheus::turn {

// The speaker a turn request names to have the engine choose one of the conversation's NPCs (decide-speaker spec). No
// actor may take it as its id.
inline constexpr std::string_view DECIDE_SPEAKER = "decide";

// What the host sends once per run (hosted-world spec, section 1): its action catalog and the JSON Schema of its
// observation, both as the host wrote them, and its lore.
struct HostData {
    nlohmann::json catalog = nlohmann::json::object();
    nlohmann::json observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> lore = {};
    // The game's clock (rollback spec, section 1): a JSON pointer to a number in every observation, e.g.
    // "/game_time/days_passed". Empty: no clock, and a run cannot be rewound.
    std::string clock = {};
    // The host's prompt texts by pack key (prompt pack spec, section 1): any of system, resolve, speak, turn_cue,
    // summarize, condense, as sent. A key left out takes the engine's default (PromptPack). Empty: no pack.
    std::map<std::string, std::string> prompts = {};
};

// HostData as a hosted run stores it and as the fixtures write it:
// {"catalog": ..., "observation_schema": ..., "lore": [{"subject": ..., "data": ..., "always_on": ...}], "clock": ...,
// "prompts": {...}};
// "clock" (a string) is written only when the host data names one; "prompts" (an object of strings) only when the host
// sent at least one text, and then only the texts it sent.
nlohmann::json HostDataToJson(const HostData& data);

// Reads HostDataToJson's form; "lore" may be absent (no lore), "clock" may be absent (no clock; when present it is a
// non-empty string), "prompts" may be absent (no pack; when present it is an object whose values are strings, its
// keys and texts checked by HostedWorld through PromptPack) and "always_on" defaults to false. Checks the shape only
// (HostedWorld checks the content): throws DataError "host data at <pointer>: <problem>".
HostData HostDataFromJson(const nlohmann::json& json);

// The content hash of `data` (core::ContentHash over HostDataToJson's text) that a hosted run stores: changed host
// data, such as a mod update, has another.
std::string HostDataHash(const HostData& data);

// An observation that passed HostedWorld::Observe: the host's JSON as sent, and its actors in their order.
struct Observation {
    nlohmann::json json = nlohmann::json::object();
    std::vector<core::Actor> actors = {};
    std::optional<double> clock = {}; // The observation's game time, when the host data names a clock.

    // The actor with `id`, or nullptr when it is not in the observation.
    const core::Actor* Find(std::string_view id) const;
    // The one actor that is the player. Throws ContractError for an observation without one (Observe refuses those).
    const core::Actor& Player() const;
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
    nlohmann::json::json_pointer _clock = nlohmann::json::json_pointer(""); // The whole document: no clock.
    bool _hasClock = false;                   // A default pointer is the whole document, so this says one was named.
    PromptPack _prompts = {};                 // The host's texts over the engine's defaults.

public:
    // Checks the host data and keeps it. Throws DataError naming the first fault: "catalog at <pointer>: ...",
    // "observation_schema: schema at <pointer>: ..." (the schema must be in grammar::Validate's extended subset) or
    // "lore at /<index>/<field>: ..." (subjects non-empty and unique, data non-empty), "host data at /clock: ..."
    // (the clock must be a well-formed JSON pointer) or PromptPack::FromHost's "prompts/<key>: ..." (the host's prompt
    // texts, checked; a text that fails is never replaced by its default).
    explicit HostedWorld(const HostData& data);

    const std::vector<core::LoreEntry>& Lore() const;

    // True when the host data names a clock: every observation then holds a finite number at its pointer.
    bool HasClock() const;

    // The run's prompt pack: the host's texts over the engine's defaults, checked.
    const PromptPack& Prompts() const;

    // Every action id of the catalog, in order: what a turn allows when the host names no allowed actions.
    std::vector<std::string> ActionIds() const;

    // Validates `observation` and returns it with its parsed core. First the core, which the engine needs whatever the
    // host's schema says: `actors`, an array of objects each with `id` and `name` (non-empty strings), `is_player`
    // (a boolean) and an optional `bio` (a string); ids unique; exactly one player. Then grammar::Validate against the
    // host's schema. Every fault of both is collected; any fault is a DataError
    // "observation at <pointer>: <problem>; observation at ...". With a clock, the observation must also hold a finite
    // number at its pointer (a fault "expected the clock, a number"), which `clock` reports. An actor whose id is
    // DECIDE_SPEAKER is a fault "'decide' is reserved: it asks the engine to choose the speaker" at its id.
    Observation Observe(const nlohmann::json& observation) const;

    // The core actors of `observation`, read as Observe reads them (core::ReadActors) but without the host's schema,
    // so a stored observation still reads after the host data changed. Throws Observe's DataError for a fault of the
    // core.
    static std::vector<core::Actor> ActorsOf(const nlohmann::json& observation);

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
