#include "core/ContentHash.h"

#include <format>

namespace morpheus::core {

void ContentHash::Add(std::string_view bytes)
{
    // FNV-1a: xor the byte in first, then multiply by the prime.
    for (const char c : bytes) {
        _value ^= static_cast<std::uint8_t>(c);
        _value *= PRIME;
    }
}

std::uint64_t ContentHash::Value() const
{
    return _value;
}

std::string ContentHash::Hex() const
{
    return std::format("{:016x}", _value);
}

} // namespace morpheus::core
