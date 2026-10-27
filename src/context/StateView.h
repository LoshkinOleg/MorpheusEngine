#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::context {

// The state view for the volatile tail (T1): one line per entity, e.g.
//   player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location "open desert"
// Entities and fields appear in name order (state JSON objects are sorted), so the text is deterministic.
// Numbers are bare, strings and enum values are quoted, list items are bare, counters print "item count".
// Precondition: `state` is validated state (the output of StateSchema::ValidateState / ApplyChanges); the field
// values are not re-checked. Throws ContractError for the shape checks it does make: the state and each entity
// must be objects, and each entity needs "type" and "fields".
std::string RenderStateView(const nlohmann::json& state);

} // namespace morpheus::context
