#include "turn/HostedWorld.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
#include "grammar/Schema.h"

namespace morpheus::turn {

namespace {

// The DataError for an observation's faults: "observation at <pointer>: <problem>", joined by "; ".
core::DataError ObservationError(const std::vector<core::ObservationFault>& faults)
{
    std::string message;
    for (const core::ObservationFault& fault : faults) {
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
        nlohmann::json written = {{"subject", entry.subject}, {"data", entry.data}, {"always_on", entry.alwaysOn}};
        // Only what the entry has, so host data without scopes and conditions keeps its hash.
        if (!entry.scopes.empty()) {
            written["scopes"] = entry.scopes;
        }
        if (entry.condition) {
            nlohmann::json condition = {{"pointer", entry.condition->pointer}};
            if (entry.condition->min) {
                condition["min"] = *entry.condition->min;
            }
            if (entry.condition->max) {
                condition["max"] = *entry.condition->max;
            }
            if (entry.condition->equals) {
                condition["equals"] = *entry.condition->equals;
            }
            written["requires"] = std::move(condition);
        }
        lore.push_back(std::move(written));
    }
    nlohmann::json json = {
        {"catalog", data.catalog}, {"observation_schema", data.observationSchema}, {"lore", std::move(lore)}};
    if (!data.clock.empty()) {
        json["clock"] = data.clock;
    }
    // Only the texts the host sent, and no key at all without one: a run without a pack keeps the hash it had.
    if (!data.prompts.empty()) {
        json["prompts"] = data.prompts;
    }
    return json;
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
        if (it.key() != "catalog" && it.key() != "observation_schema" && it.key() != "lore" &&
            it.key() != "clock" && it.key() != "prompts") {
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
    if (json.contains("clock")) {
        if (!json.at("clock").is_string()) {
            throw fault("/clock", "'clock' must be a JSON pointer string");
        }
        // An empty string would mean no clock without saying so: a run without a clock omits the key.
        if (json.at("clock").get_ref<const std::string&>().empty()) {
            throw fault("/clock", "'clock' must not be empty; a run without a clock omits it");
        }
        data.clock = json.at("clock").get<std::string>();
    }
    // The host's prompt texts: an object of strings. Its keys and texts are PromptPack's to check (HostedWorld).
    if (json.contains("prompts")) {
        const nlohmann::json& prompts = json.at("prompts");
        if (!prompts.is_object()) {
            throw fault("/prompts", "'prompts' must be a JSON object of texts");
        }
        for (auto it = prompts.begin(); it != prompts.end(); ++it) {
            if (!it.value().is_string()) {
                throw fault("/prompts/" + grammar::PointerToken(it.key()), "a prompt text must be a string");
            }
            data.prompts[it.key()] = it.value().get<std::string>();
        }
    }
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
            if (it.key() != "subject" && it.key() != "data" && it.key() != "always_on" && it.key() != "scopes" &&
                it.key() != "requires") {
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
        core::LoreEntry read;
        read.subject = entry.at("subject").get<std::string>();
        read.data = entry.at("data").get<std::string>();
        read.alwaysOn = entry.value("always_on", false);
        // Optional scopes: an array of non-empty strings.
        if (entry.contains("scopes")) {
            std::optional<std::vector<std::string>> scopes = core::ReadStringArray(entry.at("scopes"));
            if (!scopes) {
                throw fault(at + "/scopes", "'scopes' must be an array of non-empty strings");
            }
            read.scopes = std::move(*scopes);
        }
        // Optional condition: a pointer string and at least one of min, max, equals (the pointer's form is checked
        // at open, like the clock's).
        if (entry.contains("requires")) {
            const nlohmann::json& requirement = entry.at("requires");
            const std::string where = at + "/requires";
            if (!requirement.is_object()) {
                throw fault(where, "'requires' must be an object");
            }
            for (auto it = requirement.begin(); it != requirement.end(); ++it) {
                if (it.key() != "pointer" && it.key() != "min" && it.key() != "max" && it.key() != "equals") {
                    throw fault(where + "/" + grammar::PointerToken(it.key()),
                                std::format("unknown key '{}'", it.key()));
                }
            }
            if (!requirement.contains("pointer") || !requirement.at("pointer").is_string()) {
                throw fault(where + "/pointer", "'pointer' must be a JSON pointer string");
            }
            core::LoreCondition condition;
            condition.pointer = requirement.at("pointer").get<std::string>();
            for (const char* key : {"min", "max"}) {
                if (requirement.contains(key) && !requirement.at(key).is_number()) {
                    throw fault(std::format("{}/{}", where, key), std::format("'{}' must be a number", key));
                }
            }
            if (requirement.contains("min")) {
                condition.min = requirement.at("min").get<double>();
            }
            if (requirement.contains("max")) {
                condition.max = requirement.at("max").get<double>();
            }
            if (requirement.contains("equals")) {
                condition.equals = requirement.at("equals");
            }
            if (!condition.min && !condition.max && !condition.equals) {
                throw fault(where, "'requires' needs a comparison: 'min', 'max' or 'equals'");
            }
            read.condition = std::move(condition);
        }
        data.lore.push_back(std::move(read));
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

const core::Actor* Observation::Find(std::string_view id) const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [id](const core::Actor& actor) { return actor.id == id; });
    return it == actors.end() ? nullptr : &*it;
}

const core::Actor& Observation::Player() const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [](const core::Actor& actor) { return actor.isPlayer; });
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
        if (entry.alwaysOn && !entry.scopes.empty()) {
            throw core::DataError(std::format("lore at /{}/scopes: an always-on entry is in every speaker's prompt: "
                                              "scopes apply to retrieved entries",
                                              i));
        }
        // A condition's pointer must be well-formed, so Holds never meets one that is not.
        if (entry.condition) {
            try {
                const nlohmann::json::json_pointer pointer(entry.condition->pointer);
            } catch (const nlohmann::json::exception& error) {
                throw core::DataError(std::format("lore at /{}/requires/pointer: '{}' is not a JSON pointer: {}", i,
                                                  entry.condition->pointer, error.what()));
            }
        }
    }

    // 4. The clock, when named: a well-formed JSON pointer, read from every observation.
    if (!data.clock.empty()) {
        try {
            _clock = nlohmann::json::json_pointer(data.clock);
        } catch (const nlohmann::json::exception& error) {
            throw core::DataError(std::format("host data at /clock: '{}' is not a JSON pointer: {}", data.clock,
                                              error.what()));
        }
        _hasClock = true;
    }

    // 5. The prompt pack: the host's texts over the defaults, each checked (PromptPack::FromHost's DataError).
    _prompts = PromptPack::FromHost(data.prompts);
}

const std::vector<core::LoreEntry>& HostedWorld::Lore() const
{
    return _lore;
}

std::vector<core::LoreEntry> HostedWorld::AlwaysOnLore(const nlohmann::json& observation) const
{
    std::vector<core::LoreEntry> shown;
    for (const core::LoreEntry& entry : _lore) {
        // Always-on entries have no scopes, so `knows` does not matter for them.
        if (entry.alwaysOn && LoreVisible(entry, observation, {})) {
            shown.push_back(entry);
        }
    }
    return shown;
}

bool HostedWorld::LoreVisible(const core::LoreEntry& entry, const nlohmann::json& observation,
                              const std::vector<std::string>& knows)
{
    if (entry.condition && !entry.condition->Holds(observation)) {
        return false;
    }
    if (entry.scopes.empty()) {
        return true;
    }
    return std::any_of(entry.scopes.begin(), entry.scopes.end(), [&knows](const std::string& scope) {
        return std::find(knows.begin(), knows.end(), scope) != knows.end();
    });
}

std::set<std::string, std::less<>> HostedWorld::VisibleSubjects(const nlohmann::json& observation,
                                                                const std::vector<std::string>& knows) const
{
    std::set<std::string, std::less<>> subjects;
    for (const core::LoreEntry& entry : _lore) {
        if (!entry.alwaysOn && LoreVisible(entry, observation, knows)) {
            subjects.insert(entry.subject);
        }
    }
    return subjects;
}

bool HostedWorld::HasClock() const
{
    return _hasClock;
}

const PromptPack& HostedWorld::Prompts() const
{
    return _prompts;
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
    // The core first (core::ReadActors), then the host's schema; every fault of both is collected.
    core::ActorsCore read = core::ReadActors(observation);
    // No actor may take the id that asks the engine to choose the speaker.
    if (observation.is_object() && observation.contains("actors") && observation.at("actors").is_array()) {
        const nlohmann::json& actors = observation.at("actors");
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const bool reserved = actors[i].is_object() && actors[i].contains("id") && actors[i].at("id").is_string() &&
                                  actors[i].at("id").get_ref<const std::string&>() == DECIDE_SPEAKER;
            if (reserved) {
                read.faults.push_back({std::format("/actors/{}/id", i),
                                       "'decide' is reserved: it asks the engine to choose the speaker"});
            }
        }
    }
    for (const grammar::ValidationError& error : grammar::Validate(observation, _observationSchema)) {
        read.faults.push_back({error.path, error.message});
    }

    // The clock, when the host names one: a finite number at its pointer.
    std::optional<double> clock;
    if (_hasClock) {
        const bool present = observation.contains(_clock) && observation.at(_clock).is_number() &&
                             std::isfinite(observation.at(_clock).get<double>());
        if (present) {
            clock = observation.at(_clock).get<double>();
        } else {
            read.faults.push_back({_clock.to_string(), "expected the clock, a number"});
        }
    }
    if (!read.faults.empty()) {
        throw ObservationError(read.faults);
    }
    return Observation{observation, std::move(read.actors), clock};
}

std::vector<core::Actor> HostedWorld::ActorsOf(const nlohmann::json& observation)
{
    // The core alone, read as Observe reads it: a stored observation still reads after the host's schema changed.
    core::ActorsCore read = core::ReadActors(observation);
    if (!read.faults.empty()) {
        throw ObservationError(read.faults);
    }
    return std::move(read.actors);
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
        for (const core::Actor& actor : observation.actors) {
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
    // A parse failure is the model's mistake: the grammar admits text the parser refuses, such as lone surrogate
    // escapes and numbers it cannot hold (1e999).
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
