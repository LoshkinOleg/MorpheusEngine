#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace morpheus::core {

// 64-bit FNV-1a. Fingerprints world content so a saved run can tell when its world files changed.
class ContentHash {
public:
    static constexpr std::uint64_t OFFSET_BASIS = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t PRIME = 0x100000001b3ULL;

private:
    std::uint64_t _value = OFFSET_BASIS;

public:
    // Feeds more bytes. Hashing "foo" then "bar" equals hashing "foobar".
    void Add(std::string_view bytes);

    std::uint64_t Value() const;

    // 16 lower-case hex digits.
    std::string Hex() const;
};

} // namespace morpheus::core
