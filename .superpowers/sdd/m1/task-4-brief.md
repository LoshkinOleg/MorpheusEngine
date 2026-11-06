### Task 4: State changes (set / add / remove / create)

**Files:**
- Create: `src/core/Changes.h`, `src/core/Changes.cpp`, `tests/core/ChangesTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `StateSchema`, `FieldDef`, `FieldType`, `FieldTypeName`, `StateSchema::CheckFieldValue`, `StateSchema::IsValidEntityId`, `StateSchema::DefaultFields`, `StateSchema::FindEntityType` (Task 3).
- Produces (namespace `morpheus::core`):
  - `enum class ChangeOp { Set, Add, Remove, Create }`
  - `std::string ChangeOpName(ChangeOp)`
  - `struct Change { ChangeOp op; std::string path; nlohmann::json value; std::string reason; }`
  - `struct AppliedChange { Change change; std::string note; }`
  - `struct RejectedChange { nlohmann::json raw; std::string reason; }`
  - `struct ApplyResult { nlohmann::json state; std::vector<AppliedChange> applied; std::vector<RejectedChange> rejected; }`
  - `std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error)`
  - `nlohmann::json ChangeToJson(const Change&)`
  - `ApplyResult ApplyChanges(const StateSchema&, const nlohmann::json& state, const nlohmann::json& proposedChanges)`

- [ ] **Step 1: Write the failing test `tests/core/ChangesTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Errors.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

StateSchema TestSchema()
{
    return StateSchema::FromJson(nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {
                "creatable": true,
                "fields": {
                    "disposition": {"type": "enum", "values": ["wary", "friendly"]},
                    "health": {"type": "int", "min": 0, "max": 5, "default": 5}
                }
            }
        }
    })"));
}

nlohmann::json TestState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// Applies one change, given as JSON text, to TestState().
ApplyResult ApplySingle(const std::string& changeJson)
{
    return ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({nlohmann::json::parse(changeJson)}));
}

const nlohmann::json& PlayerField(const ApplyResult& result, const std::string& field)
{
    return result.state.at("player").at("fields").at(field);
}

} // namespace

TEST_CASE("set replaces a field with a valid value", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "set", "path": "player.location", "value": "bridge"})");
    REQUIRE(result.rejected.empty());
    REQUIRE(result.applied.size() == 1);
    CHECK(result.applied.at(0).change.path == "player.location");
    CHECK(PlayerField(result, "location") == "bridge");
}

TEST_CASE("set rejects anything the schema does not allow and leaves state untouched", "[changes]")
{
    const ApplyResult outOfRange = ApplySingle(R"({"op": "set", "path": "player.health", "value": 11})");
    REQUIRE(outOfRange.rejected.size() == 1);
    CHECK_THAT(outOfRange.rejected.at(0).reason, ContainsSubstring("0..10"));
    CHECK(outOfRange.state == TestState());

    CHECK(ApplySingle(R"({"op": "set", "path": "player.mood", "value": "angry"})").rejected.size() == 1);
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "ghost.health", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("unknown entity"));
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "player.wings", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("no field"));
    CHECK(ApplySingle(R"({"op": "set", "path": "player.inventory.rope", "value": 3})").rejected.size() == 1);
}

TEST_CASE("add on an int field clamps to the field range", "[changes]")
{
    const ApplyResult up = ApplySingle(R"({"op": "add", "path": "player.health", "value": 5})");
    CHECK(PlayerField(up, "health") == 10);
    CHECK(up.applied.at(0).note == "clamped to 10");

    const ApplyResult down = ApplySingle(R"({"op": "add", "path": "player.health", "value": -2})");
    CHECK(PlayerField(down, "health") == 5);
    CHECK(down.applied.at(0).note.empty());
}

TEST_CASE("add and remove on a list field", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "bruised"})"), "conditions") ==
          nlohmann::json::array({"tired", "bruised"}));

    const ApplyResult duplicate = ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "tired"})");
    CHECK(PlayerField(duplicate, "conditions") == nlohmann::json::array({"tired"}));
    CHECK(duplicate.applied.at(0).note == "already present");

    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "tired"})"), "conditions") ==
          nlohmann::json::array());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "lost"})").rejected.size() == 1);
}

TEST_CASE("add and remove on counter items", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 2})"), "inventory") ==
          nlohmann::json({{"rope", 3}}));
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": 1})"), "inventory") ==
          nlohmann::json({{"rope", 1}, {"water", 1}}));

    const ApplyResult usedUp = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -1})");
    CHECK(PlayerField(usedUp, "inventory") == nlohmann::json::object());
    CHECK(usedUp.applied.at(0).note == "removed (count reached 0)");

    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": -1})").rejected.size() == 1);
    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.inventory.rope"})"), "inventory") ==
          nlohmann::json::object());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.inventory.gold"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory", "value": 1})").rejected.size() == 1);
}

TEST_CASE("create adds entities of creatable types, filling defaults", "[changes]")
{
    const ApplyResult result = ApplySingle(
        R"({"op": "create", "path": "npc:marta", "value": {"type": "npc", "fields": {"disposition": "friendly"}}})");
    REQUIRE(result.rejected.empty());
    CHECK(result.state.at("npc:marta") ==
          nlohmann::json::parse(R"({"type": "npc", "fields": {"disposition": "friendly", "health": 5}})"));

    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "hero", "value": {"type": "character"}})").rejected.at(0).reason,
               ContainsSubstring("cannot be created"));
    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "player", "value": {"type": "npc"}})").rejected.at(0).reason,
               ContainsSubstring("already exists"));
    CHECK(ApplySingle(R"({"op": "create", "path": "Bad.Id", "value": {"type": "npc"}})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "create", "path": "npc:x", "value": {"type": "npc", "fields": {"health": 9}}})")
              .rejected.size() == 1);
}

TEST_CASE("changes see the effects of earlier changes in the same batch", "[changes]")
{
    const nlohmann::json changes = nlohmann::json::parse(R"([
        {"op": "create", "path": "npc:marta", "value": {"type": "npc"}},
        {"op": "set", "path": "npc:marta.disposition", "value": "friendly"},
        {"op": "set", "path": "player.health", "value": 99},
        {"op": "set", "path": "player.mood", "value": "tense"}
    ])");
    const ApplyResult result = ApplyChanges(TestSchema(), TestState(), changes);

    CHECK(result.applied.size() == 3);
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected.at(0).raw == changes.at(2));
    CHECK(result.state.at("npc:marta").at("fields").at("disposition") == "friendly");
    CHECK(result.state.at("player").at("fields").at("mood") == "tense");
}

TEST_CASE("malformed changes are rejected, not thrown", "[changes]")
{
    CHECK_THAT(ApplySingle(R"({"path": "player.health", "value": 1})").rejected.at(0).reason, ContainsSubstring("op"));
    CHECK(ApplySingle(R"({"op": "explode", "path": "player.health"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "set", "path": "", "value": 1})").rejected.size() == 1);
    CHECK(ApplySingle(R"("not an object")").rejected.size() == 1);
}

TEST_CASE("ApplyChanges requires an array: anything else is an engine bug", "[changes]")
{
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), TestState(), nlohmann::json::object()), ContractError);
}

TEST_CASE("ChangeToJson round-trips through ParseChange", "[changes]")
{
    const Change change{ChangeOp::Add, "player.health", -2, "fell"};
    std::string error;
    const std::optional<Change> parsed = ParseChange(ChangeToJson(change), error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->op == ChangeOp::Add);
    CHECK(parsed->path == "player.health");
    CHECK(parsed->value == -2);
    CHECK(parsed->reason == "fell");
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Changes.h'`.

- [ ] **Step 3: Implement `src/core/Changes.h`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/StateSchema.h"

namespace morpheus::core {

enum class ChangeOp { Set, Add, Remove, Create };

// Op spelling in change JSON: "set", "add", "remove" or "create".
std::string ChangeOpName(ChangeOp op);

// One proposed state change. Path forms:
// - "entity.field"       set / add / remove on a field
// - "entity.field.item"  add / remove on one item of a counter field
// - "entity"             create, with value {"type": "<creatable type>", "fields": {...optional...}}
struct Change {
    ChangeOp op = ChangeOp::Set;
    std::string path = {};
    nlohmann::json value = nullptr;
    std::string reason = {};
};

struct AppliedChange {
    Change change = {};
    std::string note = {}; // What the engine adjusted, e.g. "clamped to 0". Empty when applied exactly as proposed.
};

struct RejectedChange {
    nlohmann::json raw = nullptr; // The change exactly as proposed.
    std::string reason = {};
};

struct ApplyResult {
    nlohmann::json state = nullptr; // State after every valid change.
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
};

// Reads one change object {"op", "path", "value"?, "reason"?}. On malformed input returns nullopt and sets `error`.
std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error);

nlohmann::json ChangeToJson(const Change& change);

// Applies proposed changes, in order, to a copy of `state`. Each change is checked against the schema and the
// state as left by the changes before it; invalid ones are rejected with a reason and skipped. Rules:
// - set:    any field to a valid value. Counter item paths are not allowed (use add/remove).
// - add:    int -> adds the integer value, clamped to [min, max] (note "clamped to N");
//           list -> appends the string if absent (note "already present" otherwise);
//           counter item -> adds a non-zero integer; at 0 or below the item is removed
//           (note "removed (count reached 0)"); decreasing an absent item is rejected.
// - remove: list -> removes the string (must be present); counter item -> removes the item (must be present).
// - create: new entity id of a creatable type; missing fields take their defaults.
// `proposedChanges` must be a JSON array (ContractError otherwise); individual malformed entries are rejected.
ApplyResult ApplyChanges(const StateSchema& schema, const nlohmann::json& state, const nlohmann::json& proposedChanges);

} // namespace morpheus::core
```

- [ ] **Step 4: Implement `src/core/Changes.cpp`, and add `Changes.cpp` to `src/core/CMakeLists.txt` after `StateSchema.cpp`**

```cpp
#include "core/Changes.h"

#include <algorithm>
#include <format>

#include "core/Errors.h"

namespace morpheus::core {

std::string ChangeOpName(ChangeOp op)
{
    switch (op) {
    case ChangeOp::Set:
        return "set";
    case ChangeOp::Add:
        return "add";
    case ChangeOp::Remove:
        return "remove";
    case ChangeOp::Create:
        return "create";
    }
    throw ContractError("ChangeOpName: unhandled ChangeOp");
}

std::optional<Change> ParseChange(const nlohmann::json& raw, std::string& error)
{
    if (!raw.is_object()) {
        error = "change must be a JSON object";
        return std::nullopt;
    }
    if (!raw.contains("op") || !raw.at("op").is_string()) {
        error = "change needs a string 'op'";
        return std::nullopt;
    }
    if (!raw.contains("path") || !raw.at("path").is_string() || raw.at("path").get<std::string>().empty()) {
        error = "change needs a non-empty string 'path'";
        return std::nullopt;
    }
    if (raw.contains("reason") && !raw.at("reason").is_string()) {
        error = "change 'reason' must be a string";
        return std::nullopt;
    }

    Change change;
    const std::string opName = raw.at("op").get<std::string>();
    if (opName == "set") {
        change.op = ChangeOp::Set;
    } else if (opName == "add") {
        change.op = ChangeOp::Add;
    } else if (opName == "remove") {
        change.op = ChangeOp::Remove;
    } else if (opName == "create") {
        change.op = ChangeOp::Create;
    } else {
        error = std::format("unknown op '{}' (expected set, add, remove or create)", opName);
        return std::nullopt;
    }
    change.path = raw.at("path").get<std::string>();
    change.value = raw.contains("value") ? raw.at("value") : nlohmann::json(nullptr);
    change.reason = raw.contains("reason") ? raw.at("reason").get<std::string>() : std::string();
    return change;
}

nlohmann::json ChangeToJson(const Change& change)
{
    nlohmann::json result = {{"op", ChangeOpName(change.op)}, {"path", change.path}};
    if (!change.value.is_null()) {
        result["value"] = change.value;
    }
    if (!change.reason.empty()) {
        result["reason"] = change.reason;
    }
    return result;
}

ApplyResult ApplyChanges(const StateSchema& schema, const nlohmann::json& state, const nlohmann::json& proposedChanges)
{
    MORPHEUS_REQUIRE(proposedChanges.is_array(), "ApplyChanges expects a JSON array of changes");

    // Applies one parsed change to `target`. Returns "" on success (and may set `note`), else the rejection reason.
    // Every check happens before `target` is modified, so a rejected change never leaves partial edits.
    auto applyOne = [&schema](nlohmann::json& target, const Change& change, std::string& note) -> std::string {
        // Path segments: entity[.field[.item]]
        std::vector<std::string> parts(1);
        for (const char c : change.path) {
            if (c == '.') {
                parts.emplace_back();
            } else {
                parts.back().push_back(c);
            }
        }

        if (change.op == ChangeOp::Create) {
            if (parts.size() != 1 || !StateSchema::IsValidEntityId(change.path)) {
                return std::format("'{}' is not a valid entity id (use [a-z0-9_:])", change.path);
            }
            if (target.contains(change.path)) {
                return std::format("entity '{}' already exists", change.path);
            }
            if (!change.value.is_object() || !change.value.contains("type") || !change.value.at("type").is_string()) {
                return "create needs a value {\"type\": ..., \"fields\": {...}}";
            }
            const std::string typeName = change.value.at("type").get<std::string>();
            const EntityTypeDef* pType = schema.FindEntityType(typeName);
            if (pType == nullptr) {
                return std::format("unknown entity type '{}'", typeName);
            }
            if (!pType->creatable) {
                return std::format("entity type '{}' cannot be created", typeName);
            }

            // Start from defaults, then overlay the proposed fields after checking each one.
            nlohmann::json fields = schema.DefaultFields(typeName);
            if (change.value.contains("fields")) {
                const nlohmann::json& given = change.value.at("fields");
                if (!given.is_object()) {
                    return "create 'fields' must be an object";
                }
                for (auto it = given.begin(); it != given.end(); ++it) {
                    const auto fieldIt = pType->fields.find(it.key());
                    if (fieldIt == pType->fields.end()) {
                        return std::format("entity type '{}' has no field '{}'", typeName, it.key());
                    }
                    const std::string problem = StateSchema::CheckFieldValue(fieldIt->second, it.value());
                    if (!problem.empty()) {
                        return std::format("field '{}': {}", it.key(), problem);
                    }
                    fields[it.key()] = it.value();
                }
            }
            target[change.path] = {{"type", typeName}, {"fields", std::move(fields)}};
            return {};
        }

        // Every other op addresses a field, optionally one item of a counter field.
        if (parts.size() < 2 || parts.size() > 3) {
            return std::format("path '{}' must be entity.field or entity.field.item", change.path);
        }
        const std::string& entityId = parts[0];
        const std::string& fieldName = parts[1];
        if (!target.contains(entityId)) {
            return std::format("unknown entity '{}'", entityId);
        }
        nlohmann::json& entity = target.at(entityId);
        const EntityTypeDef* pType = schema.FindEntityType(entity.at("type").get<std::string>());
        MORPHEUS_REQUIRE(pType != nullptr, "ApplyChanges: state holds an entity of an unknown type");
        const auto fieldIt = pType->fields.find(fieldName);
        if (fieldIt == pType->fields.end()) {
            return std::format("entity '{}' has no field '{}'", entityId, fieldName);
        }
        const FieldDef& def = fieldIt->second;
        nlohmann::json& field = entity.at("fields").at(fieldName);

        const bool itemPath = parts.size() == 3;
        if (itemPath && def.type != FieldType::Counter) {
            return std::format("only counter fields take an item path; '{}' is {}", fieldName, FieldTypeName(def.type));
        }
        if (itemPath && parts[2].empty()) {
            return "counter item name is empty";
        }

        switch (change.op) {
        case ChangeOp::Set: {
            if (itemPath) {
                return "use add or remove to change a counter item";
            }
            const std::string problem = StateSchema::CheckFieldValue(def, change.value);
            if (!problem.empty()) {
                return problem;
            }
            field = change.value;
            return {};
        }
        case ChangeOp::Add:
            if (def.type == FieldType::Int) {
                if (!change.value.is_number_integer()) {
                    return "add on an int field needs an integer value";
                }
                const long long wanted = field.get<long long>() + change.value.get<long long>();
                const long long clamped = std::clamp<long long>(wanted, def.min, def.max);
                if (clamped != wanted) {
                    note = std::format("clamped to {}", clamped);
                }
                field = clamped;
                return {};
            }
            if (def.type == FieldType::List) {
                if (!change.value.is_string() || change.value.get<std::string>().empty()) {
                    return "add on a list field needs a non-empty string value";
                }
                if (std::find(field.begin(), field.end(), change.value) != field.end()) {
                    note = "already present";
                    return {};
                }
                field.push_back(change.value);
                return {};
            }
            if (def.type == FieldType::Counter) {
                if (!itemPath) {
                    return "add on a counter field needs an item path (entity.field.item)";
                }
                if (!change.value.is_number_integer() || change.value.get<long long>() == 0) {
                    return "add on a counter item needs a non-zero integer value";
                }
                const std::string& item = parts[2];
                const long long current = field.contains(item) ? field.at(item).get<long long>() : 0;
                if (current == 0 && change.value.get<long long>() < 0) {
                    return std::format("no item '{}' to decrease", item);
                }
                const long long next = current + change.value.get<long long>();
                if (next <= 0) {
                    field.erase(item);
                    note = "removed (count reached 0)";
                } else {
                    field[item] = next;
                }
                return {};
            }
            return std::format("add is not valid on a {} field", FieldTypeName(def.type));
        case ChangeOp::Remove:
            if (def.type == FieldType::List) {
                const auto it = std::find(field.begin(), field.end(), change.value);
                if (it == field.end()) {
                    return std::format("{} is not in the list", change.value.dump());
                }
                field.erase(it);
                return {};
            }
            if (def.type == FieldType::Counter) {
                if (!itemPath) {
                    return "remove on a counter field needs an item path (entity.field.item)";
                }
                if (!field.contains(parts[2])) {
                    return std::format("no item '{}'", parts[2]);
                }
                field.erase(parts[2]);
                return {};
            }
            return std::format("remove is not valid on a {} field", FieldTypeName(def.type));
        case ChangeOp::Create:
            break; // Handled above.
        }
        throw ContractError("ApplyChanges: unhandled ChangeOp");
    };

    ApplyResult result;
    result.state = state;
    for (const nlohmann::json& raw : proposedChanges) {
        std::string error;
        const std::optional<Change> change = ParseChange(raw, error);
        if (!change) {
            result.rejected.push_back({raw, error});
            continue;
        }

        std::string note;
        const std::string problem = applyOne(result.state, *change, note);
        if (!problem.empty()) {
            result.rejected.push_back({raw, problem});
            continue;
        }
        result.applied.push_back({*change, note});
    }
    return result;
}

} // namespace morpheus::core
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests/core/ChangesTests.cpp
git commit -m "Add validated state changes (set, add, remove, create)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

