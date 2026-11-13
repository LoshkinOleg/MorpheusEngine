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

// A number from low to high: each end a quarter of the time, otherwise uniform.
inline long long RandomBetween(std::mt19937_64& random, long long low, long long high)
{
    switch (std::uniform_int_distribution<int>(0, 3)(random)) {
    case 0:
        return low;
    case 1:
        return high;
    default:
        return std::uniform_int_distribution<long long>(low, high)(random);
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
    auto pick = [&](std::size_t count) { return std::uniform_int_distribution<std::size_t>(0, count - 1)(random); };

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
            if (std::uniform_int_distribution<int>(0, 1)(random) == 0) {
                text += static_cast<char>(std::uniform_int_distribution<int>(0x20, 0x7E)(random));
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
