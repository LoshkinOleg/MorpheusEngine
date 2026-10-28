#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::core {

enum class FieldType { Int, Enum, String, List, Counter };

// Schema spelling of a field type: "int", "enum", "string", "list" or "counter".
std::string FieldTypeName(FieldType type);

// One field of an entity type, as declared in state_schema.json.
struct FieldDef {
    FieldType type = FieldType::String;
    int min = 0;                           // Int only.
    int max = 0;                           // Int only.
    std::vector<std::string> values = {};  // Enum only, in declaration order.
    nlohmann::json defaultValue = nullptr; // Optional "default"; null means the type's natural default.
};

struct EntityTypeDef {
    bool creatable = false; // Whether model-proposed "create" changes may make new entities of this type.
    std::map<std::string, FieldDef> fields = {};
};

// The world's state definition (state_schema.json). State itself is plain JSON shaped as
// {"<entity id>": {"type": "<entity type>", "fields": {"<field>": <value>, ...}}, ...}.
// Field values: int -> integer in [min, max]; enum -> one of values; string -> string;
// list -> array of unique non-empty strings; counter -> object of item -> positive integer up to INT_MAX, where
// item names are non-empty and contain no '.' (items are addressed as "entity.field.item").
class StateSchema {
private:
    std::map<std::string, EntityTypeDef> _entityTypes = {};

public:
    // Parses state_schema.json ({"entity_types": {...}}). Throws DataError naming the type or field at fault.
    static StateSchema FromJson(const nlohmann::json& json);

    // Entity ids are non-empty and use only [a-z0-9_:], so they never contain the '.' path separator.
    static bool IsValidEntityId(const std::string& id);

    // Checks one value against its field definition. Returns "" when valid, otherwise a human-readable reason.
    static std::string CheckFieldValue(const FieldDef& def, const nlohmann::json& value);

    // nullptr when the type does not exist.
    const EntityTypeDef* FindEntityType(const std::string& typeName) const;

    // Every entity type, by name.
    const std::map<std::string, EntityTypeDef>& EntityTypes() const;

    // Every field of the type at its default: the declared "default", else int -> min, enum -> first value,
    // string -> "", list -> [], counter -> {}. Throws ContractError for an unknown type.
    nlohmann::json DefaultFields(const std::string& typeName) const;

    // Throws DataError unless every entity has a valid id, a known type and exactly its type's fields, all valid.
    void ValidateState(const nlohmann::json& state) const;
};

} // namespace morpheus::core
