#include "turn/HostedWorld.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
#include "grammar/Schema.h"

namespace morpheus::turn {

namespace {

// One fault of an observation: where it is ("" for the observation itself) and what is wrong.
struct Fault {
    std::string pointer = {};
    std::string problem = {};
};

// Reads the observation's core (its actors), adding a Fault for every mistake, and returns the actors read whole, in
// order. Observe and ActorsOf both read the core with it.
std::vector<Actor> ReadActors(const nlohmann::json& observation, std::vector<Fault>& faults)
{
    std::vector<Actor> actors;
    if (!observation.is_object()) {
        faults.push_back({"", "the observation must be a JSON object"});
        return actors;
    }
    if (!observation.contains("actors") || !observation.at("actors").is_array()) {
        faults.push_back({"/actors", "the observation needs 'actors', an array"});
        return actors;
    }
    const nlohmann::json& list = observation.at("actors");
    std::map<std::string, std::size_t> seen; // Actor id -> the index that has it first.
    int players = 0;                         // Actors whose is_player is true.
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = std::format("/actors/{}", i);
        const nlohmann::json& entry = list[i];
        if (!entry.is_object()) {
            faults.push_back({at, "an actor must be a JSON object"});
            continue;
        }
        // A required non-empty string field; "" with a fault when it is missing or wrong.
        auto text = [&](const char* key) {
            const bool good = entry.contains(key) && entry.at(key).is_string() &&
                              !entry.at(key).get_ref<const std::string&>().empty();
            if (!good) {
                faults.push_back({at + "/" + key, std::format("an actor needs '{}', a non-empty string", key)});
                return std::string();
            }
            return entry.at(key).get<std::string>();
        };
        Actor actor;
        actor.id = text("id");
        actor.name = text("name");
        bool whole = !actor.id.empty() && !actor.name.empty();
        if (!entry.contains("is_player") || !entry.at("is_player").is_boolean()) {
            faults.push_back({at + "/is_player", "an actor needs 'is_player', true or false"});
            whole = false;
        } else {
            actor.isPlayer = entry.at("is_player").get<bool>();
            players += actor.isPlayer ? 1 : 0;
        }
        if (entry.contains("bio")) {
            if (entry.at("bio").is_string()) {
                actor.bio = entry.at("bio").get<std::string>();
            } else {
                faults.push_back({at + "/bio", "'bio' must be a string"});
                whole = false;
            }
        }
        // Ids are unique: the engine names actors by them.
        if (!actor.id.empty()) {
            const auto [first, inserted] = seen.emplace(actor.id, i);
            if (!inserted) {
                faults.push_back({at + "/id", std::format("the id '{}' is repeated: /actors/{} has it too", actor.id,
                                                          first->second)});
                whole = false;
            }
        }
        if (whole) {
            actors.push_back(std::move(actor));
        }
    }
    if (players != 1) {
        faults.push_back({"/actors", std::format("exactly one actor must be the player, not {}", players)});
    }
    return actors;
}

// The DataError for an observation's faults: "observation at <pointer>: <problem>", joined by "; ".
core::DataError ObservationError(const std::vector<Fault>& faults)
{
    std::string message;
    for (const Fault& fault : faults) {
        message += std::format("{}observation at {}: {}", message.empty() ? "" : "; ",
                               fault.pointer.empty() ? "the root" : fault.pointer, fault.problem);
    }
    return core::DataError(message);
}

} // namespace

nlohmann::json HostDataToJson(const HostData& data)
{
    nlohmann::json lore = nlohmann::json::array();
    for (const core::LoreEntry& entry : data.lore) {
        lore.push_back({{"subject", entry.subject}, {"data", entry.data}, {"always_on", entry.alwaysOn}});
    }
    return {{"catalog", data.catalog}, {"observation_schema", data.observationSchema}, {"lore", std::move(lore)}};
}

HostData HostDataFromJson(const nlohmann::json& json)
{
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("host data at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    if (!json.is_object()) {
        throw fault("", "host data must be a JSON object");
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "catalog" && it.key() != "observation_schema" && it.key() != "lore") {
            throw fault("/" + grammar::PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
        }
    }
    if (!json.contains("catalog")) {
        throw fault("/catalog", "missing: the action catalog is required");
    }
    if (!json.contains("observation_schema")) {
        throw fault("/observation_schema", "missing: the observation's JSON Schema is required");
    }
    HostData data;
    data.catalog = json.at("catalog");
    data.observationSchema = json.at("observation_schema");
    if (!json.contains("lore")) {
        return data;
    }

    // Each lore entry: a subject and data, both strings, and an optional always_on.
    const nlohmann::json& lore = json.at("lore");
    if (!lore.is_array()) {
        throw fault("/lore", "'lore' must be an array");
    }
    for (std::size_t i = 0; i < lore.size(); ++i) {
        const std::string at = std::format("/lore/{}", i);
        const nlohmann::json& entry = lore[i];
        if (!entry.is_object()) {
            throw fault(at, "a lore entry must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "subject" && it.key() != "data" && it.key() != "always_on") {
                throw fault(at + "/" + grammar::PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
            }
        }
        if (!entry.contains("subject") || !entry.at("subject").is_string()) {
            throw fault(at + "/subject", "'subject' must be a string");
        }
        if (!entry.contains("data") || !entry.at("data").is_string()) {
            throw fault(at + "/data", "'data' must be a string");
        }
        if (entry.contains("always_on") && !entry.at("always_on").is_boolean()) {
            throw fault(at + "/always_on", "'always_on' must be true or false");
        }
        data.lore.push_back({entry.at("subject").get<std::string>(), entry.at("data").get<std::string>(),
                             entry.value("always_on", false)});
    }
    return data;
}

std::string HostDataHash(const HostData& data)
{
    // The replace handler keeps a string that is not UTF-8 from throwing; the text is the same for the same data.
    core::ContentHash hash;
    hash.Add(HostDataToJson(data).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return hash.Hex();
}

const Actor* Observation::Find(std::string_view id) const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [id](const Actor& actor) { return actor.id == id; });
    return it == actors.end() ? nullptr : &*it;
}

const Actor& Observation::Player() const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [](const Actor& actor) { return actor.isPlayer; });
    MORPHEUS_REQUIRE(it != actors.end(), "Observation::Player: an observation that passed Observe has its player");
    return *it;
}

HostedWorld::HostedWorld(const HostData& data)
    : _observationSchema(data.observationSchema)
    , _lore(data.lore)
{
    // 1. The catalog: {"actions": [...]}, each action an id and its parameters.
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("catalog at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    const nlohmann::json& catalog = data.catalog;
    if (!catalog.is_object()) {
        throw fault("", "the catalog must be a JSON object with 'actions'");
    }
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        if (it.key() != "actions") {
            throw fault("/" + grammar::PointerToken(it.key()), std::format("'{}' does not apply to the catalog", it.key()));
        }
    }
    if (!catalog.contains("actions") || !catalog.at("actions").is_array() || catalog.at("actions").empty()) {
        throw fault("/actions", "'actions' must be a non-empty array");
    }
    const nlohmann::json& actions = catalog.at("actions");
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::string at = std::format("/actions/{}", i);
        const nlohmann::json& entry = actions[i];
        if (!entry.is_object()) {
            throw fault(at, "an action must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "id" && it.key() != "params") {
                throw fault(at + "/" + grammar::PointerToken(it.key()), std::format("'{}' does not apply to an action", it.key()));
            }
        }
        if (!entry.contains("id") || !entry.at("id").is_string() || entry.at("id").get_ref<const std::string&>().empty()) {
            throw fault(at + "/id", "an action needs 'id', a non-empty string");
        }
        Action action;
        action.id = entry.at("id").get<std::string>();
        for (std::size_t j = 0; j < _actions.size(); ++j) {
            if (_actions[j].id == action.id) {
                throw fault(at + "/id", std::format("the id '{}' is repeated: /actions/{} has it too", action.id, j));
            }
        }

        // Absent params: no parameters. Each parameter is checked against its type's keys.
        if (entry.contains("params")) {
            const nlohmann::json& params = entry.at("params");
            if (!params.is_object()) {
                throw fault(at + "/params", "'params' must be a JSON object");
            }
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string paramAt = at + "/params/" + grammar::PointerToken(it.key());
                const nlohmann::json& definition = it.value();
                if (!definition.is_object()) {
                    throw fault(paramAt, "a parameter must be a JSON object");
                }
                Param param;
                param.name = it.key();
                const std::string typeName = definition.contains("type") && definition.at("type").is_string()
                                                 ? definition.at("type").get<std::string>()
                                                 : std::string();
                std::vector<std::string_view> keys; // The keys this type allows.
                std::string what;                   // The type as messages name it.
                if (typeName == "actor") {
                    param.type = ParamType::Actor;
                    keys = {"type", "scope"};
                    what = "an actor";
                } else if (typeName == "string") {
                    param.type = ParamType::String;
                    keys = {"type", "maxLength", "minLength"};
                    what = "a string";
                } else if (typeName == "integer") {
                    param.type = ParamType::Integer;
                    keys = {"type", "minimum", "maximum"};
                    what = "an integer";
                } else if (typeName == "enum") {
                    param.type = ParamType::Enum;
                    keys = {"type", "values"};
                    what = "an enum";
                } else {
                    throw fault(paramAt + "/type", "'type' must be 'actor', 'string', 'integer' or 'enum'");
                }
                for (auto key = definition.begin(); key != definition.end(); ++key) {
                    if (std::find(keys.begin(), keys.end(), key.key()) == keys.end()) {
                        throw fault(paramAt + "/" + grammar::PointerToken(key.key()),
                                    std::format("'{}' does not apply to {} parameter", key.key(), what));
                    }
                }

                switch (param.type) {
                case ParamType::Actor: {
                    // Whose ids a turn offers.
                    const std::string scope = definition.contains("scope") && definition.at("scope").is_string()
                                                  ? definition.at("scope").get<std::string>()
                                                  : std::string();
                    if (scope == "present") {
                        param.scope = Scope::Present;
                    } else if (scope == "present_non_player") {
                        param.scope = Scope::PresentNonPlayer;
                    } else if (scope == "participant") {
                        param.scope = Scope::Participant;
                    } else {
                        throw fault(paramAt + "/scope", "'scope' must be 'present', 'present_non_player' or 'participant'");
                    }
                    break;
                }
                case ParamType::String: {
                    // Bounded, so the proposal grammar is: maxLength within llama.cpp's limit, minLength up to it.
                    const std::optional<long long> maxLength =
                        definition.contains("maxLength")
                            ? core::JsonIntInRange(definition.at("maxLength"), 0, grammar::GBNF_MAX_STRING_CHARS)
                            : std::nullopt;
                    if (!maxLength) {
                        throw fault(paramAt + "/maxLength",
                                    std::format("a string parameter needs 'maxLength', an integer from 0 to {}",
                                                grammar::GBNF_MAX_STRING_CHARS));
                    }
                    param.schema = {{"type", "string"}, {"maxLength", *maxLength}};
                    if (definition.contains("minLength")) {
                        const std::optional<long long> minLength =
                            core::JsonIntInRange(definition.at("minLength"), 0, *maxLength);
                        if (!minLength) {
                            throw fault(paramAt + "/minLength",
                                        std::format("'minLength' must be an integer from 0 to {} (the 'maxLength')",
                                                    *maxLength));
                        }
                        param.schema["minLength"] = *minLength;
                    }
                    break;
                }
                case ParamType::Integer: {
                    // Both bounds, in order: the grammar writes the exact range.
                    auto bound = [&](const char* key) -> long long {
                        const std::optional<long long> value =
                            definition.contains(key)
                                ? core::JsonIntInRange(definition.at(key), std::numeric_limits<long long>::min(),
                                                       std::numeric_limits<long long>::max())
                                : std::nullopt;
                        if (!value) {
                            throw fault(paramAt + "/" + key,
                                        std::format("an integer parameter needs '{}', an integer", key));
                        }
                        return *value;
                    };
                    const long long minimum = bound("minimum");
                    const long long maximum = bound("maximum");
                    if (minimum > maximum) {
                        throw fault(paramAt, std::format("'minimum' {} is above 'maximum' {}", minimum, maximum));
                    }
                    param.schema = {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
                    break;
                }
                case ParamType::Enum: {
                    // Strings, each once.
                    const nlohmann::json values = definition.value("values", nlohmann::json());
                    const bool strings =
                        values.is_array() && !values.empty() &&
                        std::all_of(values.begin(), values.end(), [](const nlohmann::json& value) { return value.is_string(); });
                    if (!strings) {
                        throw fault(paramAt + "/values", "'values' must be a non-empty array of strings");
                    }
                    for (std::size_t k = 0; k < values.size(); ++k) {
                        for (std::size_t l = 0; l < k; ++l) {
                            if (values[l] == values[k]) {
                                throw fault(std::format("{}/values/{}", paramAt, k),
                                            std::format("'{}' is listed twice", values[k].get<std::string>()));
                            }
                        }
                    }
                    param.schema = {{"enum", values}};
                    break;
                }
                }
                action.params.push_back(std::move(param));
            }
        }
        _actions.push_back(std::move(action));
    }

    // 2. The observation schema: in Validate's extended subset, so an unsupported keyword fails here, not at a turn.
    try {
        grammar::CheckValidationSchema(data.observationSchema);
    } catch (const core::DataError& error) {
        throw core::DataError(std::format("observation_schema: {}", error.what()));
    }

    // 3. The lore: each entry has a subject, unique, and data.
    for (std::size_t i = 0; i < data.lore.size(); ++i) {
        const core::LoreEntry& entry = data.lore[i];
        if (entry.subject.empty()) {
            throw core::DataError(std::format("lore at /{}/subject: a lore entry needs a subject", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (data.lore[j].subject == entry.subject) {
                throw core::DataError(std::format("lore at /{}/subject: the subject '{}' is repeated: /{} has it too",
                                                  i, entry.subject, j));
            }
        }
        if (entry.data.empty()) {
            throw core::DataError(std::format("lore at /{}/data: a lore entry needs data", i));
        }
    }
}

const std::vector<core::LoreEntry>& HostedWorld::Lore() const
{
    return _lore;
}

std::vector<std::string> HostedWorld::ActionIds() const
{
    std::vector<std::string> ids;
    for (const Action& action : _actions) {
        ids.push_back(action.id);
    }
    return ids;
}

Observation HostedWorld::Observe(const nlohmann::json& observation) const
{
    // The core first, then the host's schema; every fault of both is collected.
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    for (const grammar::ValidationError& error : grammar::Validate(observation, _observationSchema)) {
        faults.push_back({error.path, error.message});
    }
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return Observation{observation, std::move(actors)};
}

std::vector<Actor> HostedWorld::ActorsOf(const nlohmann::json& observation)
{
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return actors;
}

void HostedWorld::CheckEvents(const std::vector<nlohmann::json>& events)
{
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!events[i].is_object()) {
            throw core::DataError(std::format("events/{}: an event must be a JSON object", i));
        }
        if (!events[i].contains("text") || !events[i].at("text").is_string()) {
            throw core::DataError(std::format("events/{}/text: an event needs 'text', a string", i));
        }
    }
}

nlohmann::json HostedWorld::ProposalSchema(const Observation& observation, const Participants& participants,
                                           const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    if (allowed.empty()) {
        throw core::DataError("allowed_actions: the list names no action");
    }
    // Every allowed id names an action of the catalog, once.
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        const bool known = std::any_of(_actions.begin(), _actions.end(),
                                       [&](const Action& action) { return action.id == allowed[i]; });
        if (!known) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is not an action of the catalog", i, allowed[i]));
        }
        if (std::find(allowed.begin(), allowed.begin() + static_cast<std::ptrdiff_t>(i), allowed[i]) !=
            allowed.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is listed twice", i, allowed[i]));
        }
    }

    // The actor ids a scope offers this turn, in observation order: never the speaker, each once.
    auto actorsIn = [&](Scope scope) {
        std::vector<std::string> ids;
        std::set<std::string> taken;
        for (const Actor& actor : observation.actors) {
            if (actor.id == speakerId) {
                continue;
            }
            const bool participates = std::any_of(participants.begin(), participants.end(),
                                                  [&](const core::Participant& p) { return p.id == actor.id; });
            const bool inScope = scope == Scope::Present || (scope == Scope::PresentNonPlayer && !actor.isPlayer) ||
                                 (scope == Scope::Participant && participates);
            if (inScope && taken.insert(actor.id).second) {
                ids.push_back(actor.id);
            }
        }
        return ids;
    };

    // One branch per allowed action the turn can offer, in catalog order.
    nlohmann::json branches = nlohmann::json::array();
    for (const Action& action : _actions) {
        if (std::find(allowed.begin(), allowed.end(), action.id) == allowed.end()) {
            continue;
        }
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        bool offered = true; // False when an actor parameter has no one to name: the action is left out.
        for (const Param& param : action.params) {
            nlohmann::json schema = param.schema;
            if (param.type == ParamType::Actor) {
                const std::vector<std::string> ids = actorsIn(param.scope);
                if (ids.empty()) {
                    offered = false;
                    break;
                }
                schema = {{"enum", ids}};
            }
            properties[param.name] = std::move(schema);
            required.push_back(param.name);
        }
        if (!offered) {
            continue;
        }
        const nlohmann::json paramsSchema = {{"type", "object"}, {"properties", properties}, {"required", required}};
        branches.push_back({{"type", "object"},
                            {"required", nlohmann::json::array({"action", "params"})},
                            {"properties", {{"action", {{"const", action.id}}}, {"params", paramsSchema}}}});
    }
    if (branches.empty()) {
        throw core::DataError("allowed_actions: no allowed action can be taken this turn: each needs an actor its "
                              "scope does not offer");
    }
    return {{"oneOf", std::move(branches)}};
}

std::string HostedWorld::ProposalGrammar(const Observation& observation, const Participants& participants,
                                         const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    return grammar::FromSchema(ProposalSchema(observation, participants, allowed, speakerId));
}

core::Adjudication HostedWorld::Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const
{
    core::Adjudication adjudication;
    // A parse failure is the model's mistake: the grammar admits lone surrogate escapes that the parser refuses.
    nlohmann::json proposal;
    try {
        proposal = nlohmann::json::parse(modelText.begin(), modelText.end());
    } catch (const nlohmann::json::exception& error) {
        adjudication.reasons.push_back(std::format("the output is not valid JSON: {}", error.what()));
        return adjudication;
    }
    // Then the proposal schema of the turn: every mistake is a reason.
    for (const grammar::ValidationError& error : grammar::Validate(proposal, proposalSchema)) {
        adjudication.reasons.push_back(std::format("{}: {}", error.path.empty() ? "the output" : error.path, error.message));
    }
    if (!adjudication.reasons.empty()) {
        return adjudication;
    }
    adjudication.requested = true;
    adjudication.actionId = proposal.at("action").get<std::string>();
    adjudication.params = proposal.at("params");
    return adjudication;
}

} // namespace morpheus::turn
