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
// - add:    int -> adds an integer value in int range, clamped to [min, max] (note "clamped to N");
//           list -> appends the string if absent (note "already present" otherwise);
//           counter item -> adds a non-zero integer in int range; the count may not exceed INT_MAX (rejected);
//           decreasing below 0 is rejected ("only N 'item' held"); reaching exactly 0 removes the item (note
//           "none left"); decreasing an absent item is rejected.
// - remove: list -> removes the string (must be present); counter item -> removes the item (must be present).
// - create: new entity id of a creatable type; missing fields take their defaults.
// `proposedChanges` must be a JSON array (ContractError otherwise); individual malformed entries are rejected.
// Precondition: `state` must already satisfy schema.ValidateState. Callers validate loaded state; a state that does
// not (missing type, fields or addressed field) throws ContractError because that is an engine bug, not bad input.
ApplyResult ApplyChanges(const StateSchema& schema, const nlohmann::json& state, const nlohmann::json& proposedChanges);

} // namespace morpheus::core
