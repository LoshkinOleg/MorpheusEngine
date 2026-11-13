#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::test {

// The property tests' fixed seed and number of draws, shared by the fast suite and the [llama] suite, so that both
// check the same values.
constexpr std::uint64_t PROPERTY_SEED = 20261114;
constexpr int PROPERTY_DRAWS = 1000;

// A number from 0 to count - 1 (count >= 1), unbiased, from the engine's raw output alone: the standard distributions
// differ between standard libraries, mt19937_64's sequence does not, so the fixed seed draws the same values everywhere.
inline std::uint64_t RandomBelow(std::mt19937_64& random, std::uint64_t count)
{
    // Rejection sampling: drop the draws below 2^64 mod count, so every remainder is equally likely.
    const std::uint64_t threshold = (0 - count) % count;
    std::uint64_t draw = random();
    while (draw < threshold) {
        draw = random();
    }
    return draw % count;
}

// A number from low to high: each end a quarter of the time, otherwise uniform.
inline long long RandomBetween(std::mt19937_64& random, long long low, long long high)
{
    switch (RandomBelow(random, 4)) {
    case 0:
        return low;
    case 1:
        return high;
    default: {
        // The span as unsigned, so the whole long long range does not overflow; 0 means all 2^64 values.
        const std::uint64_t span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low) + 1;
        const std::uint64_t offset = span == 0 ? random() : RandomBelow(random, span);
        return static_cast<long long>(static_cast<std::uint64_t>(low) + offset);
    }
    }
}

// A random value that `schema` allows (grammar::FromSchema's subset; the schema must be valid). Objects keep their
// keys in `required` order, so dump() writes the compact text the grammar expects. Strings mix printable ASCII, the
// characters JSON escapes (quote, backslash, control characters) and characters of two, three and four UTF-8 bytes;
// never U+007F, which the grammar's `char` takes only escaped while dump() writes it raw. Integers, string lengths and
// array sizes are drawn with RandomBetween, so the ends come up often and every value between them can.
inline nlohmann::ordered_json RandomValue(const nlohmann::json& schema, std::mt19937_64& random)
{
    // The characters strings are made of besides printable ASCII, as UTF-8.
    static const std::vector<std::string> SPECIAL_CHARACTERS = {
        "\"", "\\", "\n", "\t", "\r", "\b", "\f", "\x01", "\x1F", // dump() escapes these
        "\xC2\x80", "\xC3\xA9",                                   // U+0080, U+00E9: two bytes
        "\xE2\x82\xAC", "\xE6\x97\xA5",                           // U+20AC, U+65E5: three bytes
        "\xF0\x9F\x98\x80",                                       // U+1F600: four bytes
    };
    // An index from 0 to count - 1.
    auto pick = [&](std::size_t count) { return static_cast<std::size_t>(RandomBelow(random, count)); };

    if (schema.contains("oneOf")) {
        const nlohmann::json& branches = schema.at("oneOf");
        return RandomValue(branches[pick(branches.size())], random);
    }
    if (schema.contains("enum")) {
        const nlohmann::json& values = schema.at("enum");
        return nlohmann::ordered_json(values[pick(values.size())]);
    }
    if (schema.contains("const")) {
        return nlohmann::ordered_json(schema.at("const"));
    }
    const std::string type = schema.at("type").get<std::string>();
    if (type == "integer") {
        return RandomBetween(random, schema.at("minimum").get<long long>(), schema.at("maximum").get<long long>());
    }
    if (type == "string") {
        const long long length = RandomBetween(random, schema.value("minLength", 0LL), schema.at("maxLength").get<long long>());
        std::string text;
        for (long long i = 0; i < length; ++i) {
            // Half the characters printable ASCII (DEL is not printable), half from the special list.
            if (RandomBelow(random, 2) == 0) {
                text += static_cast<char>(0x20 + RandomBelow(random, 0x7F - 0x20));
            } else {
                text += SPECIAL_CHARACTERS[pick(SPECIAL_CHARACTERS.size())];
            }
        }
        return text;
    }
    if (type == "array") {
        const long long size = RandomBetween(random, schema.value("minItems", 0LL), schema.at("maxItems").get<long long>());
        nlohmann::ordered_json array = nlohmann::ordered_json::array();
        for (long long i = 0; i < size; ++i) {
            array.push_back(RandomValue(schema.at("items"), random));
        }
        return array;
    }
    // An object: every key, in `required` order.
    nlohmann::ordered_json object = nlohmann::ordered_json::object();
    for (const nlohmann::json& key : schema.at("required")) {
        const std::string name = key.get<std::string>();
        object[name] = RandomValue(schema.at("properties").at(name), random);
    }
    return object;
}

} // namespace morpheus::test
