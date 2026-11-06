### Task 3: State schema

**Files:**
- Create: `src/core/StateSchema.h`, `src/core/StateSchema.cpp`, `tests/core/StateSchemaTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError`, `ContractError`, `MORPHEUS_REQUIRE` (Task 1).
- Produces (namespace `morpheus::core`):
  - `enum class FieldType { Int, Enum, String, List, Counter }`
  - `std::string FieldTypeName(FieldType)`
  - `struct FieldDef { FieldType type; int min; int max; std::vector<std::string> values; nlohmann::json defaultValue; }`
  - `struct EntityTypeDef { bool creatable; std::map<std::string, FieldDef> fields; }`
  - `class StateSchema`:
    - `static StateSchema FromJson(const nlohmann::json&)`
    - `static bool IsValidEntityId(const std::string&)`
    - `static std::string CheckFieldValue(const FieldDef&, const nlohmann::json&)`
    - `const EntityTypeDef* FindEntityType(const std::string&) const`
    - `nlohmann::json DefaultFields(const std::string&) const`
    - `void ValidateState(const nlohmann::json&) const`
- **State JSON shape** (used by every later task): `{"<entity id>": {"type": "<entity type>", "fields": {"<field>": <value>}}}`.

- [ ] **Step 1: Write the failing test `tests/core/StateSchemaTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/StateSchema.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

nlohmann::json SchemaJson()
{
    return nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10, "default": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {"creatable": true, "fields": {"health": {"type": "int", "min": 0, "max": 5}}}
        }
    })");
}

nlohmann::json ValidState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// The schema JSON with one extra field "bad" on the character type.
nlohmann::json SchemaWithBadField(const nlohmann::json& field)
{
    nlohmann::json json = SchemaJson();
    json["entity_types"]["character"]["fields"]["bad"] = field;
    return json;
}

} // namespace

TEST_CASE("StateSchema parses entity types and fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    const EntityTypeDef* pCharacter = schema.FindEntityType("character");
    REQUIRE(pCharacter != nullptr);
    CHECK_FALSE(pCharacter->creatable);
    CHECK(pCharacter->fields.at("health").type == FieldType::Int);
    CHECK(pCharacter->fields.at("health").max == 10);
    CHECK(pCharacter->fields.at("mood").values == std::vector<std::string>{"calm", "tense"});
    CHECK(pCharacter->fields.at("inventory").type == FieldType::Counter);

    REQUIRE(schema.FindEntityType("npc") != nullptr);
    CHECK(schema.FindEntityType("npc")->creatable);
    CHECK(schema.FindEntityType("dragon") == nullptr);
}

TEST_CASE("StateSchema rejects malformed definitions", "[schema]")
{
    CHECK_THROWS_AS(StateSchema::FromJson(nlohmann::json::object()), DataError);
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "float"}})), ContainsSubstring("'bad'"));
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 5}, {"max", 1}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "enum"}, {"values", nlohmann::json::array()}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 3}, {"default", 9}})), DataError);
}

TEST_CASE("StateSchema builds default fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    CHECK(schema.DefaultFields("character") == nlohmann::json::parse(R"({
        "health": 10, "mood": "calm", "location": "", "conditions": [], "inventory": {}
    })"));
    CHECK(schema.DefaultFields("npc") == nlohmann::json::parse(R"({"health": 0})"));
    CHECK_THROWS_AS(schema.DefaultFields("dragon"), ContractError);
}

TEST_CASE("CheckFieldValue explains invalid values", "[schema]")
{
    FieldDef health;
    health.type = FieldType::Int;
    health.min = 0;
    health.max = 10;
    CHECK(StateSchema::CheckFieldValue(health, 4).empty());
    CHECK_THAT(StateSchema::CheckFieldValue(health, 11), ContainsSubstring("0..10"));
    CHECK_FALSE(StateSchema::CheckFieldValue(health, "4").empty());

    FieldDef conditions;
    conditions.type = FieldType::List;
    CHECK(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "b"})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "a"})).empty());

    FieldDef inventory;
    inventory.type = FieldType::Counter;
    CHECK(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 2}})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 0}})).empty());
}

TEST_CASE("ValidateState accepts valid state and names what is wrong otherwise", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());
    CHECK_NOTHROW(schema.ValidateState(ValidState()));

    nlohmann::json missingField = ValidState();
    missingField["player"]["fields"].erase("inventory");
    CHECK_THROWS_WITH(schema.ValidateState(missingField), ContainsSubstring("inventory"));

    nlohmann::json extraField = ValidState();
    extraField["player"]["fields"]["wings"] = 2;
    CHECK_THROWS_WITH(schema.ValidateState(extraField), ContainsSubstring("wings"));

    nlohmann::json badValue = ValidState();
    badValue["player"]["fields"]["health"] = 99;
    CHECK_THROWS_WITH(schema.ValidateState(badValue), ContainsSubstring("health"));

    nlohmann::json badType = ValidState();
    badType["player"]["type"] = "dragon";
    CHECK_THROWS_WITH(schema.ValidateState(badType), ContainsSubstring("dragon"));

    nlohmann::json badId = ValidState();
    badId["Bad.Id"] = badId["player"];
    CHECK_THROWS_AS(schema.ValidateState(badId), DataError);
}

TEST_CASE("Entity ids never contain the path separator", "[schema]")
{
    CHECK(StateSchema::IsValidEntityId("player"));
    CHECK(StateSchema::IsValidEntityId("npc:old_marta"));
    CHECK_FALSE(StateSchema::IsValidEntityId(""));
    CHECK_FALSE(StateSchema::IsValidEntityId("a.b"));
    CHECK_FALSE(StateSchema::IsValidEntityId("Player"));
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/StateSchema.h'`.

- [ ] **Step 3: Implement `src/core/StateSchema.h`**

```cpp
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
// list -> array of unique non-empty strings; counter -> object of item -> positive integer.
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

    // Every field of the type at its default: the declared "default", else int -> min, enum -> first value,
    // string -> "", list -> [], counter -> {}. Throws ContractError for an unknown type.
    nlohmann::json DefaultFields(const std::string& typeName) const;

    // Throws DataError unless every entity has a valid id, a known type and exactly its type's fields, all valid.
    void ValidateState(const nlohmann::json& state) const;
};

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/StateSchema.cpp`, and add `StateSchema.cpp` to `src/core/CMakeLists.txt` after `Csv.cpp`**

```cpp
#include "core/StateSchema.h"

#include <algorithm>
#include <format>
#include <set>

#include "core/Errors.h"

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
            if (typeText == "int") {
                def.type = FieldType::Int;
                if (!fieldJson.contains("min") || !fieldJson.at("min").is_number_integer() ||
                    !fieldJson.contains("max") || !fieldJson.at("max").is_number_integer()) {
                    throw DataError(fieldWhere + ": int fields need integer 'min' and 'max'");
                }
                def.min = fieldJson.at("min").get<int>();
                def.max = fieldJson.at("max").get<int>();
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
            } else if (typeText == "counter") {
                def.type = FieldType::Counter;
            } else {
                throw DataError(std::format("{}: unknown type '{}' (expected int, enum, string, list or counter)",
                                            fieldWhere, typeText));
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
    case FieldType::Int:
        if (!value.is_number_integer()) {
            return std::format("expected an integer, got {}", value.dump());
        }
        if (value.get<long long>() < def.min || value.get<long long>() > def.max) {
            return std::format("{} is outside {}..{}", value.get<long long>(), def.min, def.max);
        }
        return {};
    case FieldType::Enum:
        if (!value.is_string() ||
            std::find(def.values.begin(), def.values.end(), value.get<std::string>()) == def.values.end()) {
            return std::format("expected one of {}, got {}", nlohmann::json(def.values).dump(), value.dump());
        }
        return {};
    case FieldType::String:
        if (!value.is_string()) {
            return std::format("expected a string, got {}", value.dump());
        }
        return {};
    case FieldType::List: {
        if (!value.is_array()) {
            return std::format("expected an array of strings, got {}", value.dump());
        }
        std::set<std::string> seen;
        for (const nlohmann::json& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return std::format("list items must be non-empty strings, got {}", item.dump());
            }
            if (!seen.insert(item.get<std::string>()).second) {
                return std::format("list item {} appears twice", item.dump());
            }
        }
        return {};
    }
    case FieldType::Counter:
        if (!value.is_object()) {
            return std::format("expected an object of item counts, got {}", value.dump());
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.value().is_number_integer() || it.value().get<long long>() <= 0) {
                return std::format("count of '{}' must be a positive integer, got {}", it.key(), it.value().dump());
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
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/StateSchemaTests.cpp
git commit -m "Add state schema with field validation and defaults" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

