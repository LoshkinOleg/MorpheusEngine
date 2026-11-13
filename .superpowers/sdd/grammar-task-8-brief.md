### Task 8: The property test in the fast suite

`RandomValue` draws values a schema allows, with a fixed seed. In the fast suite, 1000 values from the catalog schema (and 1000 from the every-keyword schema) pass `Validate`, and the matcher accepts each one's compact text against `FromSchema`'s grammar. A third test checks that the draws really reach every action, every size and the range ends.

**Files:**
- Create: `tests/grammar/RandomValue.h`
- Test: `tests/grammar/PropertyTests.cpp`

**Interfaces:**
- Consumes: `grammar::FromSchema`, `grammar::Validate` (Tasks 6, 7); `test::CatalogSchema`, `test::EveryKeywordSchema`; `test::GbnfMatcher`; `core::Utf8Length`.
- Produces (header `grammar/RandomValue.h`, namespace `morpheus::test`): `constexpr std::uint64_t PROPERTY_SEED = 20261114;`, `constexpr int PROPERTY_DRAWS = 1000;`, `long long RandomBetween(std::mt19937_64& random, long long low, long long high)`, `nlohmann::ordered_json RandomValue(const nlohmann::json& schema, std::mt19937_64& random)`. Task 9 uses `PROPERTY_SEED`, `PROPERTY_DRAWS` and `RandomValue`.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/PropertyTests.cpp`:

```cpp
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
            escaped = escaped || item.find_first_of("\"\\\n\t") != std::string::npos;
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/RandomValue.h': No such file or directory` in `tests/grammar/PropertyTests.cpp`.

- [ ] **Step 3: Write the helper**

Create `tests/grammar/RandomValue.h`:

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[property]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). If the coverage test fails (for example no 48-character item came up), the draws do not exercise what the spec asks: change the helper's distribution, not the seed.

- [ ] **Step 5: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 357`.

- [ ] **Step 6: Commit**

```bash
git add tests/grammar/RandomValue.h tests/grammar/PropertyTests.cpp
git commit -m "Add the schema property test: random values pass Validate and the grammar" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

