#include "core/HostedRecord.h"

#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <utility>

namespace morpheus::core {

ActorsCore ReadActors(const nlohmann::json& observation)
{
    ActorsCore read;
    // The observation itself, then its `actors` array: without either, nothing more can be read.
    if (!observation.is_object()) {
        read.faults.push_back({"", "the observation must be a JSON object"});
        return read;
    }
    if (!observation.contains("actors") || !observation.at("actors").is_array()) {
        read.faults.push_back({"/actors", "the observation needs 'actors', an array"});
        return read;
    }
    const nlohmann::json& list = observation.at("actors");
    std::map<std::string, std::size_t> seen; // Actor id -> the index that has it first.
    int players = 0;                         // Actors whose is_player is true.
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = std::format("/actors/{}", i);
        const nlohmann::json& entry = list[i];
        if (!entry.is_object()) {
            read.faults.push_back({at, "an actor must be a JSON object"});
            continue;
        }
        // A required non-empty string field; "" with a fault when it is missing or wrong.
        auto text = [&](const char* key) {
            const bool good = entry.contains(key) && entry.at(key).is_string() &&
                              !entry.at(key).get_ref<const std::string&>().empty();
            if (!good) {
                read.faults.push_back({at + "/" + key, std::format("an actor needs '{}', a non-empty string", key)});
                return std::string();
            }
            return entry.at(key).get<std::string>();
        };
        Actor actor;
        actor.id = text("id");
        actor.name = text("name");
        bool whole = !actor.id.empty() && !actor.name.empty();
        if (!entry.contains("is_player") || !entry.at("is_player").is_boolean()) {
            read.faults.push_back({at + "/is_player", "an actor needs 'is_player', true or false"});
            whole = false;
        } else {
            actor.isPlayer = entry.at("is_player").get<bool>();
            players += actor.isPlayer ? 1 : 0;
        }
        if (entry.contains("bio")) {
            if (entry.at("bio").is_string()) {
                actor.bio = entry.at("bio").get<std::string>();
            } else {
                read.faults.push_back({at + "/bio", "'bio' must be a string"});
                whole = false;
            }
        }
        if (entry.contains("knows")) {
            if (std::optional<std::vector<std::string>> knows = ReadStringArray(entry.at("knows"))) {
                actor.knows = std::move(*knows);
            } else {
                read.faults.push_back({at + "/knows", "'knows' must be an array of non-empty strings"});
                whole = false;
            }
        }
        // Ids are unique: the engine names actors by them.
        if (!actor.id.empty()) {
            const auto [first, inserted] = seen.emplace(actor.id, i);
            if (!inserted) {
                read.faults.push_back({at + "/id", std::format("the id '{}' is repeated: /actors/{} has it too",
                                                               actor.id, first->second)});
                whole = false;
            }
        }
        if (whole) {
            read.actors.push_back(std::move(actor));
        }
    }
    if (players != 1) {
        read.faults.push_back({"/actors", std::format("exactly one actor must be the player, not {}", players)});
    }
    return read;
}

std::optional<std::vector<std::string>> ReadStringArray(const nlohmann::json& value)
{
    if (!value.is_array()) {
        return std::nullopt;
    }
    for (const nlohmann::json& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return std::nullopt;
        }
    }
    return value.get<std::vector<std::string>>();
}

} // namespace morpheus::core
