#include "core/StateSchema.h"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "core/Errors.h"
#include "core/JsonInt.h"
#include "core/Text.h"

namespace morpheus::core {

std::string FieldTypeName(FieldType type)
{
    switch (type) {
    case FieldType::Int:
        return "int";
    case FieldType::Enum:
        return "enum";
    case FieldType::String:
        return "string";
    case FieldType::List:
        return "list";
    case FieldType::Counter:
        return "counter";
    }
    throw ContractError("FieldTypeName: unhandled FieldType");
}

StateSchema StateSchema::FromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("entity_types") || !json.at("entity_types").is_object() ||
        json.at("entity_types").empty()) {
        throw DataError("state schema: needs a non-empty 'entity_types' object");
    }

    // Rejects any key of `object` outside `allowed`, so a typo fails loudly instead of being ignored.
    auto rejectUnknownKeys = [](const nlohmann::json& object, const std::set<std::string>& allowed,
                                const std::string& where) {
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!allowed.contains(it.key())) {
                throw DataError(std::format("{}: unknown key '{}'", where, it.key()));
            }
        }
    };
    rejectUnknownKeys(json, {"entity_types"}, "state schema");

    // Entity type and field names are identifiers: lower-case letters, digits and underscores.
    auto isIdentifier = [](const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    };

    StateSchema schema;
    for (const auto& [typeName, typeJson] : json.at("entity_types").items()) {
        const std::string where = std::format("state schema: entity type '{}'", typeName);
        if (!isIdentifier(typeName)) {
            throw DataError(where + ": name must use only [a-z0-9_]");
        }
        if (!typeJson.is_object()) {
            throw DataError(where + ": must be an object");
        }

        rejectUnknownKeys(typeJson, {"creatable", "fields"}, where);

        EntityTypeDef typeDef;
        if (typeJson.contains("creatable")) {
            if (!typeJson.at("creatable").is_boolean()) {
                throw DataError(where + ": 'creatable' must be true or false");
            }
            typeDef.creatable = typeJson.at("creatable").get<bool>();
        }
        if (!typeJson.contains("fields") || !typeJson.at("fields").is_object() || typeJson.at("fields").empty()) {
            throw DataError(where + ": needs a non-empty 'fields' object");
        }

        for (const auto& [fieldName, fieldJson] : typeJson.at("fields").items()) {
            const std::string fieldWhere = std::format("{}: field '{}'", where, fieldName);
            if (!isIdentifier(fieldName)) {
                throw DataError(fieldWhere + ": name must use only [a-z0-9_]");
            }
            if (!fieldJson.is_object() || !fieldJson.contains("type") || !fieldJson.at("type").is_string()) {
                throw DataError(fieldWhere + ": needs a string 'type'");
            }

            // Type-specific attributes: int needs a range, enum needs its values.
            FieldDef def;
            const std::string typeText = fieldJson.at("type").get<std::string>();

            // The type comes first: every other complaint about this field depends on knowing it.
            const std::set<std::string> knownTypes = {"int", "enum", "string", "list", "counter"};
            if (!knownTypes.contains(typeText)) {
                throw DataError(std::format("{}: unknown type '{}' (expected int, enum, string, list or counter)",
                                            fieldWhere, typeText));
            }

            // Only keys that mean something for this field type are allowed ("min" on a string is a mistake).
            std::set<std::string> allowedKeys = {"type", "default"};
            if (typeText == "int") {
                allowedKeys.insert({"min", "max"});
            } else if (typeText == "enum") {
                allowedKeys.insert("values");
            }
            rejectUnknownKeys(fieldJson, allowedKeys, fieldWhere);

            if (typeText == "int") {
                def.type = FieldType::Int;
                if (!fieldJson.contains("min") || !fieldJson.at("min").is_number_integer() ||
                    !fieldJson.contains("max") || !fieldJson.at("max").is_number_integer()) {
                    throw DataError(fieldWhere + ": int fields need integer 'min' and 'max'");
                }
                // Reject bounds outside int before converting, so they cannot wrap silently.
                int* const bounds[] = {&def.min, &def.max};
                const char* const boundNames[] = {"min", "max"};
                for (int i = 0; i < 2; ++i) {
                    const nlohmann::json& bound = fieldJson.at(boundNames[i]);
                    const std::optional<long long> parsed =
                        JsonIntInRange(bound, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
                    if (!parsed) {
                        throw DataError(std::format("{}: '{}' {} is outside the int range {}..{}", fieldWhere,
                                                    boundNames[i], bound.dump(), std::numeric_limits<int>::min(),
                                                    std::numeric_limits<int>::max()));
                    }
                    *bounds[i] = static_cast<int>(*parsed);
                }
                if (def.min > def.max) {
                    throw DataError(fieldWhere + ": 'min' is greater than 'max'");
                }
            } else if (typeText == "enum") {
                def.type = FieldType::Enum;
                if (!fieldJson.contains("values") || !fieldJson.at("values").is_array() ||
                    fieldJson.at("values").empty()) {
                    throw DataError(fieldWhere + ": enum fields need a non-empty 'values' array");
                }
                std::set<std::string> seen;
                for (const nlohmann::json& value : fieldJson.at("values")) {
                    if (!value.is_string() || value.get<std::string>().empty() ||
                        !seen.insert(value.get<std::string>()).second) {
                        throw DataError(fieldWhere + ": enum values must be unique non-empty strings");
                    }
                    def.values.push_back(value.get<std::string>());
                }
            } else if (typeText == "string") {
                def.type = FieldType::String;
            } else if (typeText == "list") {
                def.type = FieldType::List;
            } else {
                // Only "counter" is left: knownTypes was checked above.
                def.type = FieldType::Counter;
            }

            // An explicit default must itself be a valid value.
            if (fieldJson.contains("default")) {
                const std::string problem = CheckFieldValue(def, fieldJson.at("default"));
                if (!problem.empty()) {
                    throw DataError(fieldWhere + ": invalid default: " + problem);
                }
                def.defaultValue = fieldJson.at("default");
            }
            typeDef.fields.emplace(fieldName, std::move(def));
        }
        schema._entityTypes.emplace(typeName, std::move(typeDef));
    }
    return schema;
}

bool StateSchema::IsValidEntityId(const std::string& id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':';
    });
}

std::string StateSchema::CheckFieldValue(const FieldDef& def, const nlohmann::json& value)
{
    switch (def.type) {
    case FieldType::Int: {
        if (!value.is_number_integer()) {
            return std::format("expected an integer, got {}", ShortJson(value));
        }
        // Range-check through the shared helper, which never wraps a huge unsigned value.
        if (!JsonIntInRange(value, def.min, def.max)) {
            return std::format("{} is outside {}..{}", ShortJson(value), def.min, def.max);
        }
        return {};
    }
    case FieldType::Enum:
        if (!value.is_string() ||
            std::find(def.values.begin(), def.values.end(), value.get<std::string>()) == def.values.end()) {
            return std::format("expected one of {}, got {}", nlohmann::json(def.values).dump(), ShortJson(value));
        }
        return {};
    case FieldType::String:
        if (!value.is_string()) {
            return std::format("expected a string, got {}", ShortJson(value));
        }
        return {};
    case FieldType::List: {
        if (!value.is_array()) {
            return std::format("expected an array of strings, got {}", ShortJson(value));
        }
        std::set<std::string> seen;
        for (const nlohmann::json& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return std::format("list items must be non-empty strings, got {}", ShortJson(item));
            }
            if (!seen.insert(item.get<std::string>()).second) {
                return std::format("list item {} appears twice", ShortJson(item));
            }
        }
        return {};
    }
    case FieldType::Counter:
        if (!value.is_object()) {
            return std::format("expected an object of item counts, got {}", ShortJson(value));
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key().empty() || it.key().find('.') != std::string::npos) {
                return std::format("counter item names must be non-empty and contain no '.', got '{}'", it.key());
            }
            const nlohmann::json& count = it.value();
            const bool inRange = JsonIntInRange(count, 1, std::numeric_limits<int>::max()).has_value();
            if (!inRange) {
                return std::format("count of '{}' must be an integer in 1..{}, got {}", it.key(),
                                   std::numeric_limits<int>::max(), ShortJson(count));
            }
        }
        return {};
    }
    throw ContractError("CheckFieldValue: unhandled FieldType");
}

const EntityTypeDef* StateSchema::FindEntityType(const std::string& typeName) const
{
    const auto it = _entityTypes.find(typeName);
    return it == _entityTypes.end() ? nullptr : &it->second;
}

const std::map<std::string, EntityTypeDef>& StateSchema::EntityTypes() const
{
    return _entityTypes;
}

nlohmann::json StateSchema::DefaultFields(const std::string& typeName) const
{
    const EntityTypeDef* pType = FindEntityType(typeName);
    MORPHEUS_REQUIRE(pType != nullptr, std::format("DefaultFields: unknown entity type '{}'", typeName));

    nlohmann::json fields = nlohmann::json::object();
    for (const auto& [name, def] : pType->fields) {
        if (!def.defaultValue.is_null()) {
            fields[name] = def.defaultValue;
            continue;
        }
        switch (def.type) {
        case FieldType::Int:
            fields[name] = def.min;
            break;
        case FieldType::Enum:
            fields[name] = def.values.front();
            break;
        case FieldType::String:
            fields[name] = "";
            break;
        case FieldType::List:
            fields[name] = nlohmann::json::array();
            break;
        case FieldType::Counter:
            fields[name] = nlohmann::json::object();
            break;
        }
    }
    return fields;
}

void StateSchema::ValidateState(const nlohmann::json& state) const
{
    if (!state.is_object()) {
        throw DataError("state: must be an object of entities");
    }

    for (const auto& [entityId, entity] : state.items()) {
        const std::string where = std::format("state: entity '{}'", entityId);
        if (!IsValidEntityId(entityId)) {
            throw DataError(where + ": ids use only [a-z0-9_:]");
        }
        if (!entity.is_object() || !entity.contains("type") || !entity.at("type").is_string() ||
            !entity.contains("fields") || !entity.at("fields").is_object()) {
            throw DataError(where + ": must be {\"type\": ..., \"fields\": {...}}");
        }

        const std::string typeName = entity.at("type").get<std::string>();
        const EntityTypeDef* pType = FindEntityType(typeName);
        if (pType == nullptr) {
            throw DataError(std::format("{}: unknown type '{}'", where, typeName));
        }

        // Every declared field must be present and valid...
        const nlohmann::json& fields = entity.at("fields");
        for (const auto& [fieldName, def] : pType->fields) {
            if (!fields.contains(fieldName)) {
                throw DataError(std::format("{}: missing field '{}'", where, fieldName));
            }
            const std::string problem = CheckFieldValue(def, fields.at(fieldName));
            if (!problem.empty()) {
                throw DataError(std::format("{}: field '{}': {}", where, fieldName, problem));
            }
        }
        // ...and nothing undeclared may be present.
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!pType->fields.contains(it.key())) {
                throw DataError(std::format("{}: unknown field '{}'", where, it.key()));
            }
        }
    }
}

} // namespace morpheus::core
