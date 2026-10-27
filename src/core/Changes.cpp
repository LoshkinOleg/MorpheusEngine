#include "core/Changes.h"

#include <algorithm>
#include <format>
#include <limits>

#include "core/Errors.h"
#include "core/JsonInt.h"

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

namespace {

// Bounds of the integers a model may add: anything that fits an int.
constexpr long long INT_MIN_VALUE = std::numeric_limits<int>::min();
constexpr long long INT_MAX_VALUE = std::numeric_limits<int>::max();

} // namespace

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
                const std::optional<long long> delta = JsonIntInRange(change.value, INT_MIN_VALUE, INT_MAX_VALUE);
                if (!delta) {
                    return std::format("add on an int field needs an integer in int range, got {}", change.value.dump());
                }
                // Both operands are int-ranged, so the sum fits in long long.
                const long long wanted = field.get<long long>() + *delta;
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
                const std::optional<long long> parsedDelta = JsonIntInRange(change.value, INT_MIN_VALUE, INT_MAX_VALUE);
                if (!parsedDelta || *parsedDelta == 0) {
                    return std::format("add on a counter item needs a non-zero integer in int range, got {}",
                                       change.value.dump());
                }
                const long long delta = *parsedDelta;
                const std::string& item = parts[2];
                const long long current = field.contains(item) ? field.at(item).get<long long>() : 0;
                if (current == 0 && delta < 0) {
                    return std::format("no item '{}' to decrease", item);
                }
                // Both operands are int-ranged, so the sum fits in long long.
                const long long next = current + delta;
                if (next > std::numeric_limits<int>::max()) {
                    return std::format("count of '{}' would exceed {}", item, std::numeric_limits<int>::max());
                }
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
