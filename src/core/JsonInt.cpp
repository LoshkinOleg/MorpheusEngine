#include "core/JsonInt.h"

#include <cstdint>
#include <limits>

#include "core/Errors.h"

namespace morpheus::core {

std::optional<long long> JsonIntInRange(const nlohmann::json& value, long long lo, long long hi)
{
    MORPHEUS_REQUIRE(lo <= hi, "JsonIntInRange: lo must not exceed hi");

    // Unsigned first: a uint64 above LLONG_MAX would wrap negative through get<long long>().
    if (value.is_number_unsigned()) {
        const std::uint64_t number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
            return std::nullopt;
        }
        const long long converted = static_cast<long long>(number);
        return converted >= lo && converted <= hi ? std::optional<long long>(converted) : std::nullopt;
    }

    // Floats and bools are not integers (nlohmann reports is_number_integer() false for both).
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    const long long number = value.get<long long>();
    return number >= lo && number <= hi ? std::optional<long long>(number) : std::nullopt;
}

} // namespace morpheus::core
