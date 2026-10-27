#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::context {

// The state view for the volatile tail (T1): one line per entity, e.g.
//   player (captain): conditions [], health 10, inventory {compass 1, pistol 1}, location "open desert"
// Entities and fields appear in name order (state JSON objects are sorted), so the text is deterministic.
// Numbers are bare, strings and enum values are quoted, list items are bare, counters print "item count".
// Throws ContractError for a state that is not shaped like validated state.
std::string RenderStateView(const nlohmann::json& state);

} // namespace morpheus::context
