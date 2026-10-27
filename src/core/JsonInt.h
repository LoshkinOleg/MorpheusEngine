#pragma once

#include <optional>

#include <nlohmann/json.hpp>

namespace morpheus::core {

// Returns the value when `value` is a JSON integer (not a float or bool) within [lo, hi], otherwise nullopt.
// Unsigned values are range-checked before conversion, so uint64 values above LLONG_MAX can never wrap.
std::optional<long long> JsonIntInRange(const nlohmann::json& value, long long lo, long long hi);

} // namespace morpheus::core
