#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Text.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/RandomValue.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;

namespace {

// Draws test::PROPERTY_DRAWS values from `schema` with the fixed seed and checks that each, read back from its compact
// text as a host reads the model's answer, passes Validate, and that the text fits the schema's grammar.
void CheckRandomValues(const nlohmann::json& schema)
{
    const test::GbnfMatcher matcher(grammar::FromSchema(schema));
    std::mt19937_64 random(test::PROPERTY_SEED);
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const std::string text = test::RandomValue(schema, random).dump();
        INFO("value " << i << ": " << text);
        const std::vector<grammar::ValidationError> errors = grammar::Validate(nlohmann::json::parse(text), schema);
        for (const grammar::ValidationError& error : errors) {
            UNSCOPED_INFO(error.path << ": " << error.message);
        }
        CHECK(errors.empty());
        CHECK(matcher.Accepts(text));
    }
}

} // namespace

TEST_CASE("Random catalog actions pass Validate and the catalog grammar", "[property]")
{
    CheckRandomValues(test::CatalogSchema());
}

TEST_CASE("Random values of every keyword pass Validate and their grammar", "[property]")
{
    CheckRandomValues(test::EveryKeywordSchema());
}

TEST_CASE("RandomValue reaches every action, size and range end the schemas allow", "[property]")
{
    // The same draws as the property tests: the same seed, the same order.
    const nlohmann::json catalog = test::CatalogSchema();
    std::mt19937_64 random(test::PROPERTY_SEED);
    std::set<std::string> actions;
    std::set<std::size_t> itemLengths;
    bool nonAscii = false;
    bool escaped = false;
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const nlohmann::ordered_json value = test::RandomValue(catalog, random);
        actions.insert(value.at("action").get<std::string>());
        if (value.at("action") == "give_item") {
            const std::string item = value.at("params").at("item").get<std::string>();
            itemLengths.insert(core::Utf8Length(item));
            nonAscii = nonAscii || std::any_of(item.begin(), item.end(), [](char c) {
                           return (static_cast<unsigned char>(c) & 0x80) != 0;
                       });
            escaped = escaped || item.find_first_of("\"\\n\t") != std::string::npos;
        }
    }
    CHECK(actions == std::set<std::string>{"none", "follow", "wait", "attack", "give_item", "end_conversation"});
    CHECK(itemLengths.contains(0));
    CHECK(itemLengths.contains(48));
    CHECK(nonAscii);
    CHECK(escaped);

    const nlohmann::json every = test::EveryKeywordSchema();
    std::mt19937_64 everyRandom(test::PROPERTY_SEED);
    std::set<std::size_t> tagSizes;
    std::set<std::size_t> codeSizes;
    std::set<std::size_t> nameLengths;
    std::set<long long> counts;
    std::set<long long> bigs;
    std::set<std::string> modes;
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const nlohmann::ordered_json value = test::RandomValue(every, everyRandom);
        tagSizes.insert(value.at("tags").size());
        codeSizes.insert(value.at("codes").size());
        nameLengths.insert(core::Utf8Length(value.at("name").get<std::string>()));
        counts.insert(value.at("count").get<long long>());
        bigs.insert(value.at("big").get<long long>());
        modes.insert(value.at("mode").at("state").get<std::string>());
    }
    CHECK(tagSizes == std::set<std::size_t>{1, 2, 3});
    CHECK(codeSizes == std::set<std::size_t>{0, 1, 2});
    CHECK(nameLengths == std::set<std::size_t>{2, 3, 4, 5, 6});
    CHECK(counts.contains(-20));
    CHECK(counts.contains(120));
    CHECK(bigs.contains(std::numeric_limits<long long>::min()));
    CHECK(bigs.contains(std::numeric_limits<long long>::max()));
    CHECK(modes == std::set<std::string>{"on", "off"});
}
