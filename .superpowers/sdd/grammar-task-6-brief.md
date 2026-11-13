### Task 6: `FromSchema`

One internal step, `Compile`, reads a schema into a checked tree (`Node`) and throws `DataError` naming the JSON pointer; `FromSchema` writes the tree with the builder. Task 7 adds `Validate` over the same tree.

**Files:**
- Create: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`, `tests/grammar/TestSchemas.h`
- Modify: `src/grammar/CMakeLists.txt`
- Test: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: the builder (Tasks 3, 4); `core::DataError`, `core::ContractError` (`core/Errors.h`); `core::JsonIntInRange` (`core/JsonInt.h`); `test::GbnfMatcher`.
- Produces:
  - `std::string grammar::FromSchema(const nlohmann::json& schema);` (header `grammar/Schema.h`).
  - In `Schema.cpp` (anonymous namespace, used again by Task 7): `struct Node` (kinds `Object`, `Enum`, `Const`, `Integer`, `String`, `Array`, `OneOf`; fields `keys`, `children`, `values`, `minimum`, `maximum`, `minCount`, `maxCount`), `std::string PointerToken(std::string_view key)`, `Node Compile(const nlohmann::json& schema, const std::string& pointer)`.
  - `tests/grammar/TestSchemas.h`: `nlohmann::json test::CatalogSchema()`, `nlohmann::json test::EveryKeywordSchema()`.

- [ ] **Step 1: Write the test schemas**

Create `tests/grammar/TestSchemas.h`:

```cpp
#pragma once

#include <nlohmann/json.hpp>

namespace morpheus::test {

// The host's example action catalog (integration requirements, section 5.2) as a schema of FromSchema's subset: one
// branch per action, told apart by "action". attack's target is one of the present non-player actors (Hulda
// 0001A6A4 and Saadia 0001B07C), give_item's item a string of at most 48 characters, and the actions without
// parameters have empty params objects. It carries ignored annotations, as a host's schema may.
inline nlohmann::json CatalogSchema()
{
    return nlohmann::json::parse(R"({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "action",
        "oneOf": [
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "none"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "follow"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "additionalProperties": false, "properties": {
                "action": {"const": "wait"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "description": "Attack a present actor.", "properties": {
                "action": {"const": "attack"},
                "params": {"type": "object", "required": ["target"], "properties": {
                    "target": {"enum": ["0001A6A4", "0001B07C"]}}}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "give_item"},
                "params": {"type": "object", "required": ["item"], "properties": {
                    "item": {"type": "string", "maxLength": 48}}}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "end_conversation"},
                "params": {"type": "object", "properties": {}, "required": []}}}
        ]
    })");
}

// A schema that uses every keyword of the subset: integers (one over the whole long long range), a string with both
// lengths, arrays with and without minItems over string and integer enums, an integer const, an array of objects, an
// empty object and a nested oneOf. Its `required` orders are not alphabetical, so key order is tested too.
inline nlohmann::json EveryKeywordSchema()
{
    return nlohmann::json::parse(R"({
        "title": "Every keyword of the subset",
        "type": "object",
        "properties": {
            "count": {"type": "integer", "minimum": -20, "maximum": 120},
            "big": {"type": "integer", "minimum": -9223372036854775808, "maximum": 9223372036854775807},
            "name": {"type": "string", "minLength": 2, "maxLength": 6, "description": "Lengths count characters."},
            "tags": {"type": "array", "items": {"enum": ["red", "green", "blue"]}, "minItems": 1, "maxItems": 3},
            "codes": {"type": "array", "items": {"enum": [3, -7, 120]}, "maxItems": 2},
            "version": {"const": 2},
            "pair": {"type": "array", "minItems": 2, "maxItems": 2, "items": {
                "type": "object", "required": ["x", "label"], "properties": {
                    "x": {"type": "integer", "minimum": 0, "maximum": 9},
                    "label": {"const": "point"}}}},
            "empty": {"type": "object", "properties": {}, "required": [], "additionalProperties": false},
            "mode": {"oneOf": [
                {"type": "object", "required": ["state", "level"], "properties": {
                    "state": {"const": "on"},
                    "level": {"type": "integer", "minimum": 1, "maximum": 3}}},
                {"type": "object", "required": ["state"], "properties": {
                    "state": {"const": "off"}}}
            ]}
        },
        "required": ["name", "count", "big", "tags", "codes", "version", "pair", "empty", "mode"]
    })");
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing tests**

Create `tests/grammar/SchemaTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;
using namespace nlohmann::literals;

namespace {

// The grammar GrammarBuilder writes for the root expression `rootBody`: what FromSchema must give.
std::string GrammarOf(const std::string& rootBody)
{
    return grammar::GrammarBuilder().Text(rootBody);
}

// The message of the DataError FromSchema throws for the schema `text`.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        return error.what();
    }
    FAIL("FromSchema accepted the schema " << text);
    return {};
}

// A oneOf branch with the one key `key`, whose schema is the JSON text `tag`.
std::string Branch(const std::string& key, const std::string& tag)
{
    return R"({"type": "object", "properties": {")" + key + R"(": )" + tag + R"(}, "required": [")" + key + R"("]})";
}

// 48 characters of two bytes each: the longest item the catalog allows.
std::string LongestItem()
{
    std::string item;
    for (int i = 0; i < 48; ++i) {
        item += "\xC3\xA9";
    }
    return item;
}

} // namespace

TEST_CASE("FromSchema writes each keyword with the builder", "[schema]")
{
    CHECK(grammar::FromSchema(R"({"const": "none"})"_json) == GrammarOf(grammar::Literal(R"("none")")));
    CHECK(grammar::FromSchema(R"({"const": -3})"_json) == GrammarOf(grammar::Literal("-3")));
    CHECK(grammar::FromSchema(R"({"enum": ["a", "b"]})"_json) == GrammarOf(grammar::Enum({"a", "b"})));
    CHECK(grammar::FromSchema(R"({"enum": [1, 20]})"_json) ==
          GrammarOf(grammar::Alternatives({grammar::Literal("1"), grammar::Literal("20")})));
    CHECK(grammar::FromSchema(R"({"type": "integer", "minimum": -5, "maximum": 300})"_json) ==
          GrammarOf(grammar::IntRange(-5, 300)));
    CHECK(grammar::FromSchema(R"({"type": "string", "maxLength": 5})"_json) == GrammarOf(grammar::JsonString(0, 5)));
    CHECK(grammar::FromSchema(R"({"type": "string", "minLength": 2, "maxLength": 5})"_json) ==
          GrammarOf(grammar::JsonString(2, 5)));
    CHECK(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "maxItems": 2})"_json) ==
          GrammarOf(grammar::ListOf(grammar::Literal("1"), 0, 2)));
    CHECK(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "minItems": 1, "maxItems": 3})"_json) ==
          GrammarOf(grammar::ListOf(grammar::Literal("1"), 1, 3)));
    // Keys follow `required`, not the sorted order nlohmann keeps `properties` in; no keys is the empty object.
    CHECK(grammar::FromSchema(
              R"({"type": "object", "properties": {"b": {"const": 1}, "a": {"const": 2}}, "required": ["b", "a"]})"_json) ==
          GrammarOf(grammar::Object({{"b", grammar::Literal("1")}, {"a", grammar::Literal("2")}})));
    CHECK(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": []})"_json) ==
          GrammarOf(grammar::Literal("{}")));
    CHECK(grammar::FromSchema(nlohmann::json::parse(R"({"oneOf": [)" + Branch("k", R"({"const": "x"})") + ", " +
                                                    Branch("k", R"({"const": "y"})") + "]}")) ==
          GrammarOf(grammar::Alternatives({grammar::Object({{"k", grammar::Literal(R"("x")")}}),
                                           grammar::Object({{"k", grammar::Literal(R"("y")")}})})));
    // One exact text pins the shape: the root line first, and a JSON string brings the shared `char` rule.
    CHECK(grammar::FromSchema(
              R"({"type": "object", "properties": {"name": {"type": "string", "maxLength": 8}}, "required": ["name"]})"_json) ==
          R"gbnf(root ::= "{\"name\":" "\"" char{0,8} "\"" "}"
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf");
}

TEST_CASE("FromSchema ignores the keywords that do not change values", "[schema]")
{
    const nlohmann::json annotated = R"({
        "$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:test", "title": "t",
        "description": "d", "$comment": "c",
        "type": "object", "properties": {"a": {"description": "the a", "const": 1}}, "required": ["a"]
    })"_json;
    CHECK(grammar::FromSchema(annotated) ==
          grammar::FromSchema(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": ["a"]})"_json));
    // additionalProperties false says what the subset always means.
    CHECK(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": false})"_json) ==
          grammar::FromSchema(R"({"type": "object", "properties": {}, "required": []})"_json));
}

TEST_CASE("The catalog grammar accepts the host's actions and refuses its model's mistakes", "[schema]")
{
    const test::GbnfMatcher matcher(grammar::FromSchema(test::CatalogSchema()));
    const std::string longest = LongestItem();
    const std::vector<std::string> accepted = {
        R"({"action":"none","params":{}})",
        R"({"action":"follow","params":{}})",
        R"({"action":"wait","params":{}})",
        R"({"action":"attack","params":{"target":"0001A6A4"}})",
        R"({"action":"attack","params":{"target":"0001B07C"}})",
        R"({"action":"give_item","params":{"item":"Iron Sword"}})",
        R"({"action":"give_item","params":{"item":""}})",
        R"({"action":"give_item","params":{"item":")" + longest + R"("}})",
        R"({"action":"end_conversation","params":{}})",
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(matcher.Accepts(text));
    }
    const std::vector<std::string> rejected = {
        R"({"action":"dance","params":{}})",                                // an unknown action
        R"({"action":"attack","params":{"target":"0001C3D2"}})",            // an actor who is not present
        R"({"action":"attack","params":{"target":"Saadia"}})",              // a name, not an id
        R"({"action":"attack","params":{}})",                               // a missing argument
        R"({"action":"give_item","params":{"item":")" + longest + R"(a"}})", // 49 characters
        R"({"action":"wait","params":{},"intent":"rest"})",                 // an extra key (the measured Qwen failure)
        R"({"action":"wait","params":{"intent":"rest"}})",                  // an extra argument
        R"({"params":{},"action":"wait"})",                                 // keys out of order
        R"({"action":"wait"})",                                             // no params
        R"({"action": "wait", "params": {}})",                              // whitespace
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }
}

TEST_CASE("The grammar of every keyword accepts a valid value and refuses each broken one", "[schema]")
{
    const test::GbnfMatcher matcher(grammar::FromSchema(test::EveryKeywordSchema()));
    const std::string base =
        R"({"name":"ab","count":0,"big":5,"tags":["red"],"codes":[],"version":2,"pair":[{"x":1,"label":"point"},{"x":2,"label":"point"}],"empty":{},"mode":{"state":"off"}})";
    // `base` with its first `from` replaced by `to`.
    auto with = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    };
    CHECK(matcher.Accepts(base));
    const std::vector<std::string> accepted = {
        with(R"("mode":{"state":"off"})", R"("mode":{"state":"on","level":3})"),
        with(R"("big":5)", R"("big":-9223372036854775808)"),
        with(R"("big":5)", R"("big":9223372036854775807)"),
        with(R"("count":0)", R"("count":-20)"),
        with(R"("count":0)", R"("count":120)"),
        with(R"("codes":[])", R"("codes":[-7,120])"),
        with(R"("tags":["red"])", R"("tags":["red","green","blue"])"),
        with(R"("name":"ab")", R"("name":"\u00e9\"")"),
        with(R"("name":"ab")", "\"name\":\"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\""),
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(matcher.Accepts(text));
    }
    const std::vector<std::string> rejected = {
        with(R"("count":0)", R"("count":121)"),
        with(R"("count":0)", R"("count":-21)"),
        with(R"("big":5)", R"("big":9223372036854775808)"),
        with(R"("tags":["red"])", R"("tags":[])"),
        with(R"("tags":["red"])", R"("tags":["pink"])"),
        with(R"("codes":[])", R"("codes":[3,3,3])"),
        with(R"("codes":[])", R"("codes":[4])"),
        with(R"("version":2)", R"("version":3)"),
        with(R"(,{"x":2,"label":"point"})", ""),                          // pair has exactly two items
        with(R"({"x":1,"label":"point"})", R"({"label":"point","x":1})"), // keys out of order
        with(R"("empty":{})", R"("empty":{"a":1})"),
        with(R"("mode":{"state":"off"})", R"("mode":{"state":"on"})"),
        with(R"("name":"ab")", R"("name":"a")"),
        with(R"("name":"ab")", R"("name":"abcdefg")"),
        with(R"({"name":"ab","count":0)", R"({"count":0,"name":"ab")"),
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }
}

TEST_CASE("FromSchema refuses what is not a schema of the subset, naming the pointer", "[schema]")
{
    CHECK(SchemaError("[]") == "schema at the root: a schema must be a JSON object");
    CHECK(SchemaError("{}") == "schema at the root: needs 'type', 'enum', 'const' or 'oneOf'");
    CHECK(SchemaError(R"({"type": "string", "maxLength": 3, "enum": ["a"]})") ==
          "schema at the root: 'type' and 'enum' cannot be combined");
    CHECK(SchemaError(R"({"type": "string", "maxLength": 3, "pattern": "a+"})") ==
          "schema at /pattern: the keyword 'pattern' is not supported");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"$ref": "#/$defs/a"}}, "required": ["a"]})") ==
          "schema at /properties/a/$ref: the keyword '$ref' is not supported");
    CHECK(SchemaError(R"({"const": "a", "description": "fine", "default": "a"})") ==
          "schema at /default: the keyword 'default' is not supported");
    CHECK(SchemaError(R"({"type": "boolean"})") == "schema at /type: the type 'boolean' is not supported");
    CHECK(SchemaError(R"({"type": "number"})") == "schema at /type: the type 'number' is not supported");
    CHECK(SchemaError(R"({"type": ["string", "null"], "maxLength": 3})") == "schema at /type: 'type' must be a string");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 3, "maxLength": 2})") ==
          "schema at /maxLength: 'maxLength' does not apply to an integer");
}

TEST_CASE("FromSchema refuses objects outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "object", "required": []})") ==
          "schema at the root: an object needs 'properties', an object");
    CHECK(SchemaError(R"({"type": "object", "properties": {}})") ==
          "schema at the root: an object needs 'required', an array");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})") ==
          "schema at /properties/a: 'a' must be in 'required': optional keys are not supported");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": ["a"]})") ==
          "schema at /required/0: 'a' is required but not in 'properties'");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": ["a", "a"]})") ==
          "schema at /required/1: 'a' is listed twice");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": [1]})") ==
          "schema at /required/0: 'required' entries must be strings");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})") ==
          "schema at /additionalProperties: 'additionalProperties' may only be false: extra keys are never allowed");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": [], "minItems": 1})") ==
          "schema at /minItems: 'minItems' does not apply to an object");
    // A key with "/" and "~" is escaped in the pointer as RFC 6901 says.
    CHECK(SchemaError(R"({"type": "object", "properties": {"a/b~c": {"type": "string"}}, "required": ["a/b~c"]})") ==
          "schema at /properties/a~1b~0c: a string needs 'maxLength'");
}

TEST_CASE("FromSchema refuses enums outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"enum": []})") == "schema at /enum: 'enum' must be a non-empty array");
    CHECK(SchemaError(R"({"enum": "a"})") == "schema at /enum: 'enum' must be a non-empty array");
    CHECK(SchemaError(R"({"enum": ["a", 1]})") == "schema at /enum/1: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": [1, 2.5]})") == "schema at /enum/1: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": [true]})") == "schema at /enum/0: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": ["a", "b", "a"]})") == R"(schema at /enum/2: "a" is listed twice)");
    CHECK(SchemaError(R"({"enum": ["a"], "maxLength": 3})") == "schema at /maxLength: 'maxLength' does not apply to an enum");
}

TEST_CASE("FromSchema refuses consts outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"const": 1.5})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": null})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": {"a": 1}})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": "a", "maxLength": 3})") == "schema at /maxLength: 'maxLength' does not apply to a const");
}

TEST_CASE("FromSchema refuses integers outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0})") ==
          "schema at the root: an integer needs 'minimum' and 'maximum'");
    CHECK(SchemaError(R"({"type": "integer", "maximum": 0})") ==
          "schema at the root: an integer needs 'minimum' and 'maximum'");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 5, "maximum": 3})") ==
          "schema at the root: 'minimum' 5 is above 'maximum' 3");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0.5, "maximum": 3})") ==
          "schema at /minimum: 'minimum' must be an integer from -9223372036854775808 to 9223372036854775807");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 18446744073709551615})") ==
          "schema at /maximum: 'maximum' must be an integer from -9223372036854775808 to 9223372036854775807");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 3, "exclusiveMaximum": 4})") ==
          "schema at /exclusiveMaximum: the keyword 'exclusiveMaximum' is not supported");
}

TEST_CASE("FromSchema refuses strings outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "string"})") == "schema at the root: a string needs 'maxLength'");
    CHECK(SchemaError(R"({"type": "string", "minLength": 4, "maxLength": 3})") ==
          "schema at the root: 'minLength' 4 is above 'maxLength' 3");
    CHECK(SchemaError(R"({"type": "string", "maxLength": -1})") ==
          "schema at /maxLength: 'maxLength' must be an integer from 0 to 2147483647");
    CHECK(SchemaError(R"({"type": "string", "maxLength": "3"})") ==
          "schema at /maxLength: 'maxLength' must be an integer from 0 to 2147483647");
}

TEST_CASE("FromSchema refuses arrays outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "array", "maxItems": 3})") == "schema at the root: an array needs 'items'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}})") == "schema at the root: an array needs 'maxItems'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 0})") ==
          "schema at /maxItems: 'maxItems' must be at least 1");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "minItems": 4, "maxItems": 3})") ==
          "schema at the root: 'minItems' 4 is above 'maxItems' 3");
    CHECK(SchemaError(R"({"type": "array", "items": {"type": "string"}, "maxItems": 3})") ==
          "schema at /items: a string needs 'maxLength'");
}

TEST_CASE("FromSchema refuses oneOfs outside the subset", "[schema]")
{
    const std::string x = Branch("action", R"({"const": "x"})");
    const std::string y = Branch("action", R"({"const": "y"})");
    CHECK(SchemaError(R"({"oneOf": []})") == "schema at /oneOf: 'oneOf' must be a non-empty array");
    CHECK(SchemaError(R"({"oneOf": [)" + x + R"(, {"const": "x"}]})") ==
          "schema at /oneOf/1: a 'oneOf' branch must be an object schema");
    CHECK(SchemaError(R"({"oneOf": [{"type": "object", "properties": {}, "required": []}]})") ==
          "schema at /oneOf/0/required: a 'oneOf' branch needs a discriminator, the first key of 'required'");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("kind", R"({"const": "y"})") + "]}") ==
          "schema at /oneOf/1/required/0: the discriminator is 'kind' here but 'action' in /oneOf/0");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("action", R"({"enum": ["y"]})") + "]}") ==
          "schema at /oneOf/1/properties/action: the discriminator 'action' must be a 'const' string");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("action", R"({"const": 1})") + "]}") ==
          "schema at /oneOf/1/properties/action: the discriminator 'action' must be a 'const' string");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + y + ", " + x + "]}") ==
          "schema at /oneOf/2/properties/action/const: the discriminator value 'x' is repeated: /oneOf/0 has it too");
    CHECK(SchemaError(R"({"type": "object", "oneOf": [)" + x + "]}") ==
          "schema at the root: 'type' and 'oneOf' cannot be combined");
}

TEST_CASE("FromSchema names the pointer of a mistake deep inside the schema", "[schema]")
{
    // The catalog's attack target with no maxLength: the spec's example pointer.
    nlohmann::json schema = test::CatalogSchema();
    schema["oneOf"][3]["properties"]["params"]["properties"]["target"] = nlohmann::json::parse(R"({"type": "string"})");
    CHECK(SchemaError(schema.dump()) ==
          "schema at /oneOf/3/properties/params/properties/target: a string needs 'maxLength'");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Schema.h': No such file or directory` in `tests/grammar/SchemaTests.cpp`.

- [ ] **Step 4: Add the source to the library**

In `src/grammar/CMakeLists.txt`, replace

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
)
```

with

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
    Schema.cpp
)
```

- [ ] **Step 5: Write the header**

Create `src/grammar/Schema.h`:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::grammar {

// A complete GBNF grammar (root line first), built with GrammarBuilder, for the compact JSON values `schema` allows.
// The schema is a subset of JSON Schema, exactly this:
//   {"type": "object", "properties": {...}, "required": [...]}  every property required; keys in `required` order;
//                                                               "additionalProperties" absent or false (extra keys
//                                                               are never allowed); empty properties and required
//                                                               give {}
//   {"enum": [...]}                                             all strings, or all integers
//   {"const": v}                                                a string or an integer
//   {"type": "integer", "minimum": a, "maximum": b}             both bounds, a <= b, exact
//   {"type": "string", "maxLength": n}                          optional "minLength"; lengths in characters
//   {"type": "array", "items": {...}, "maxItems": n}            n >= 1, optional "minItems"
//   {"oneOf": [...]}                                            every branch an object schema; the discriminator is
//                                                               the first key of each branch's `required`, the same
//                                                               key in every branch, a const string in each, and
//                                                               different across branches
// "title", "description", "$comment", "$schema" and "$id" are allowed anywhere and ignored. Nested schemas follow the
// same rules at any depth; "$ref" is not supported. Anything else is a core::DataError (schemas are host data) whose
// message names the JSON pointer inside the schema and the problem: "schema at <pointer>: <problem>".
std::string FromSchema(const nlohmann::json& schema);

} // namespace morpheus::grammar
```

- [ ] **Step 6: Write the implementation**

Create `src/grammar/Schema.cpp`:

```cpp
#include "grammar/Schema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"

namespace morpheus::grammar {

namespace {

// Keywords that do not change the values a schema allows: accepted anywhere and ignored.
constexpr std::array<std::string_view, 5> IGNORED_KEYWORDS = {"title", "description", "$comment", "$schema", "$id"};
// Every keyword of the subset; any other is refused by name.
constexpr std::array<std::string_view, 14> SUBSET_KEYWORDS = {
    "type",    "enum",    "const",     "oneOf",     "properties", "required", "additionalProperties",
    "minimum", "maximum", "minLength", "maxLength", "items",      "minItems", "maxItems"};
// The keywords that say what a value is; a schema has exactly one of them.
constexpr std::array<std::string_view, 4> KIND_KEYWORDS = {"type", "enum", "const", "oneOf"};

// A schema checked against the subset, as FromSchema and Validate read it.
struct Node {
    enum class Kind { Object, Enum, Const, Integer, String, Array, OneOf };

    Kind kind = Kind::Object;
    std::vector<std::string> keys = {};      // Object: its keys, in `required` order.
    std::vector<Node> children = {};         // Object: one value schema per key; Array: the item schema;
                                             // OneOf: the branches, each an Object whose first key is the discriminator.
    std::vector<nlohmann::json> values = {}; // Enum: the allowed values (all strings or all integers); Const: the one.
    long long minimum = 0;                   // Integer: the bounds, both included.
    long long maximum = 0;
    int minCount = 0;                        // String: characters; Array: items.
    int maxCount = 0;
};

// `key` as one reference token of a JSON pointer (RFC 6901): "~" is written "~0" and "/" is written "~1".
std::string PointerToken(std::string_view key)
{
    std::string token;
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

// Reads `schema`, found at `pointer` inside the whole schema, into a Node, checking it against the subset. Throws
// DataError naming the pointer of the mistake. Recursive: the schemas of an object's properties, an array's items and
// a oneOf's branches follow the same rules.
Node Compile(const nlohmann::json& schema, const std::string& pointer)
{
    auto error = [](const std::string& at, const std::string& problem) {
        return core::DataError(std::format("schema at {}: {}", at.empty() ? "the root" : at, problem));
    };
    auto contains = [](const auto& keywords, const std::string& key) {
        return std::find(keywords.begin(), keywords.end(), key) != keywords.end();
    };
    if (!schema.is_object()) {
        throw error(pointer, "a schema must be a JSON object");
    }

    // Keywords outside the subset are named first, so that "$ref" or "pattern" is reported as such.
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!contains(SUBSET_KEYWORDS, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
            throw error(pointer + "/" + PointerToken(it.key()), std::format("the keyword '{}' is not supported", it.key()));
        }
    }
    // Exactly one keyword says what the value is.
    std::vector<std::string> kinds;
    for (const std::string_view keyword : KIND_KEYWORDS) {
        if (schema.contains(std::string(keyword))) {
            kinds.emplace_back(keyword);
        }
    }
    if (kinds.empty()) {
        throw error(pointer, "needs 'type', 'enum', 'const' or 'oneOf'");
    }
    if (kinds.size() > 1) {
        throw error(pointer, std::format("'{}' and '{}' cannot be combined", kinds[0], kinds[1]));
    }
    const std::string kind = kinds[0];

    // Only this kind's keywords, and the ignored ones, may appear.
    auto allowOnly = [&](std::string_view what, std::initializer_list<std::string_view> allowed) {
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            if (!contains(allowed, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
                throw error(pointer + "/" + PointerToken(it.key()), std::format("'{}' does not apply to {}", it.key(), what));
            }
        }
    };
    // A bound keyword: absent (nullopt), or a JSON integer from low to high.
    auto integer = [&](const char* keyword, long long low, long long high) -> std::optional<long long> {
        if (!schema.contains(keyword)) {
            return std::nullopt;
        }
        const std::optional<long long> value = core::JsonIntInRange(schema.at(keyword), low, high);
        if (!value) {
            throw error(pointer + "/" + keyword, std::format("'{}' must be an integer from {} to {}", keyword, low, high));
        }
        return value;
    };
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();
    constexpr long long MOST_COUNT = std::numeric_limits<int>::max();

    Node node;
    if (kind == "oneOf") {
        allowOnly("a oneOf", {"oneOf"});
        const nlohmann::json& branches = schema.at("oneOf");
        const std::string at = pointer + "/oneOf";
        if (!branches.is_array() || branches.empty()) {
            throw error(at, "'oneOf' must be a non-empty array");
        }
        node.kind = Node::Kind::OneOf;
        std::map<std::string, std::string> taken; // Discriminator value -> the branch that has it.
        for (std::size_t i = 0; i < branches.size(); ++i) {
            const nlohmann::json& branch = branches[i];
            const std::string branchAt = std::format("{}/{}", at, i);
            if (!branch.is_object() || !branch.contains("type") || branch.at("type") != "object") {
                throw error(branchAt, "a 'oneOf' branch must be an object schema");
            }
            Node compiled = Compile(branch, branchAt);
            // The discriminator: the first key of the branch's `required`, the same key in every branch, and a const
            // string in each that no other branch has.
            if (compiled.keys.empty()) {
                throw error(branchAt + "/required", "a 'oneOf' branch needs a discriminator, the first key of 'required'");
            }
            const std::string& key = compiled.keys[0];
            if (i > 0 && key != node.children[0].keys[0]) {
                throw error(branchAt + "/required/0", std::format("the discriminator is '{}' here but '{}' in {}/0", key,
                                                                  node.children[0].keys[0], at));
            }
            const Node& tag = compiled.children[0];
            const std::string tagAt = branchAt + "/properties/" + PointerToken(key);
            if (tag.kind != Node::Kind::Const || !tag.values[0].is_string()) {
                throw error(tagAt, std::format("the discriminator '{}' must be a 'const' string", key));
            }
            const std::string value = tag.values[0].get<std::string>();
            if (const auto found = taken.find(value); found != taken.end()) {
                throw error(tagAt + "/const",
                            std::format("the discriminator value '{}' is repeated: {} has it too", value, found->second));
            }
            taken.emplace(value, branchAt);
            node.children.push_back(std::move(compiled));
        }
        return node;
    }

    if (kind == "enum") {
        allowOnly("an enum", {"enum"});
        const nlohmann::json& values = schema.at("enum");
        const std::string at = pointer + "/enum";
        if (!values.is_array() || values.empty()) {
            throw error(at, "'enum' must be a non-empty array");
        }
        // All strings or all integers (a bool is neither), each listed once.
        const bool strings = values[0].is_string();
        for (std::size_t i = 0; i < values.size(); ++i) {
            const nlohmann::json& value = values[i];
            const std::string valueAt = std::format("{}/{}", at, i);
            if (strings ? !value.is_string() : !value.is_number_integer()) {
                throw error(valueAt, "'enum' values must be all strings or all integers");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (values[j] == value) {
                    throw error(valueAt, std::format("{} is listed twice", value.dump()));
                }
            }
        }
        node.kind = Node::Kind::Enum;
        node.values.assign(values.begin(), values.end());
        return node;
    }

    if (kind == "const") {
        allowOnly("a const", {"const"});
        const nlohmann::json& value = schema.at("const");
        if (!value.is_string() && !value.is_number_integer()) {
            throw error(pointer + "/const", "'const' must be a string or an integer");
        }
        node.kind = Node::Kind::Const;
        node.values.push_back(value);
        return node;
    }

    // The rest is told by "type".
    const nlohmann::json& type = schema.at("type");
    if (!type.is_string()) {
        throw error(pointer + "/type", "'type' must be a string");
    }
    const std::string typeName = type.get<std::string>();

    if (typeName == "integer") {
        allowOnly("an integer", {"type", "minimum", "maximum"});
        const std::optional<long long> minimum = integer("minimum", LOWEST, HIGHEST);
        const std::optional<long long> maximum = integer("maximum", LOWEST, HIGHEST);
        if (!minimum || !maximum) {
            throw error(pointer, "an integer needs 'minimum' and 'maximum'");
        }
        if (*minimum > *maximum) {
            throw error(pointer, std::format("'minimum' {} is above 'maximum' {}", *minimum, *maximum));
        }
        node.kind = Node::Kind::Integer;
        node.minimum = *minimum;
        node.maximum = *maximum;
        return node;
    }

    if (typeName == "string") {
        allowOnly("a string", {"type", "minLength", "maxLength"});
        const std::optional<long long> minLength = integer("minLength", 0, MOST_COUNT);
        const std::optional<long long> maxLength = integer("maxLength", 0, MOST_COUNT);
        if (!maxLength) {
            throw error(pointer, "a string needs 'maxLength'");
        }
        if (minLength.value_or(0) > *maxLength) {
            throw error(pointer, std::format("'minLength' {} is above 'maxLength' {}", *minLength, *maxLength));
        }
        node.kind = Node::Kind::String;
        node.minCount = static_cast<int>(minLength.value_or(0));
        node.maxCount = static_cast<int>(*maxLength);
        return node;
    }

    if (typeName == "array") {
        allowOnly("an array", {"type", "items", "minItems", "maxItems"});
        if (!schema.contains("items")) {
            throw error(pointer, "an array needs 'items'");
        }
        const std::optional<long long> minItems = integer("minItems", 0, MOST_COUNT);
        const std::optional<long long> maxItems = integer("maxItems", 0, MOST_COUNT);
        if (!maxItems) {
            throw error(pointer, "an array needs 'maxItems'");
        }
        if (*maxItems < 1) {
            throw error(pointer + "/maxItems", "'maxItems' must be at least 1");
        }
        if (minItems.value_or(0) > *maxItems) {
            throw error(pointer, std::format("'minItems' {} is above 'maxItems' {}", *minItems, *maxItems));
        }
        node.kind = Node::Kind::Array;
        node.minCount = static_cast<int>(minItems.value_or(0));
        node.maxCount = static_cast<int>(*maxItems);
        node.children.push_back(Compile(schema.at("items"), pointer + "/items"));
        return node;
    }

    if (typeName == "object") {
        allowOnly("an object", {"type", "properties", "required", "additionalProperties"});
        if (schema.contains("additionalProperties") && schema.at("additionalProperties") != false) {
            throw error(pointer + "/additionalProperties",
                        "'additionalProperties' may only be false: extra keys are never allowed");
        }
        if (!schema.contains("properties") || !schema.at("properties").is_object()) {
            throw error(pointer, "an object needs 'properties', an object");
        }
        if (!schema.contains("required") || !schema.at("required").is_array()) {
            throw error(pointer, "an object needs 'required', an array");
        }
        const nlohmann::json& properties = schema.at("properties");
        const nlohmann::json& required = schema.at("required");
        node.kind = Node::Kind::Object;
        // The keys in `required` order, which the grammar writes them in (JSON objects carry no order).
        for (std::size_t i = 0; i < required.size(); ++i) {
            const std::string at = std::format("{}/required/{}", pointer, i);
            if (!required[i].is_string()) {
                throw error(at, "'required' entries must be strings");
            }
            const std::string key = required[i].get<std::string>();
            if (!properties.contains(key)) {
                throw error(at, std::format("'{}' is required but not in 'properties'", key));
            }
            if (std::find(node.keys.begin(), node.keys.end(), key) != node.keys.end()) {
                throw error(at, std::format("'{}' is listed twice", key));
            }
            node.keys.push_back(key);
            node.children.push_back(Compile(properties.at(key), pointer + "/properties/" + PointerToken(key)));
        }
        // Every property is required: optional keys are outside the subset.
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                throw error(pointer + "/properties/" + PointerToken(it.key()),
                            std::format("'{}' must be in 'required': optional keys are not supported", it.key()));
            }
        }
        return node;
    }

    throw error(pointer + "/type", std::format("the type '{}' is not supported", typeName));
}

// The grammar expression for the values `node` allows. Recursive, like Compile.
std::string Expression(const Node& node)
{
    switch (node.kind) {
    case Node::Kind::Object: {
        // An object without keys is the empty object (Object needs at least one field).
        if (node.keys.empty()) {
            return Literal("{}");
        }
        std::vector<ObjectField> fields;
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            fields.push_back({node.keys[i], Expression(node.children[i])});
        }
        return Object(fields);
    }
    case Node::Kind::Enum: {
        // Strings as JSON strings (Enum); integers as their decimal text.
        if (node.values[0].is_string()) {
            std::vector<std::string> strings;
            for (const nlohmann::json& value : node.values) {
                strings.push_back(value.get<std::string>());
            }
            return Enum(strings);
        }
        std::vector<std::string> numbers;
        for (const nlohmann::json& value : node.values) {
            numbers.push_back(Literal(value.dump()));
        }
        return Alternatives(numbers);
    }
    case Node::Kind::Const:
        return Literal(node.values[0].dump());
    case Node::Kind::Integer:
        return IntRange(node.minimum, node.maximum);
    case Node::Kind::String:
        return JsonString(node.minCount, node.maxCount);
    case Node::Kind::Array:
        return ListOf(Expression(node.children[0]), node.minCount, node.maxCount);
    case Node::Kind::OneOf: {
        std::vector<std::string> branches;
        for (const Node& branch : node.children) {
            branches.push_back(Expression(branch));
        }
        return Alternatives(branches);
    }
    }
    throw core::ContractError("FromSchema: unhandled schema kind");
}

} // namespace

std::string FromSchema(const nlohmann::json& schema)
{
    // Check the whole schema first, then write it: a bad schema never yields part of a grammar.
    const Node root = Compile(schema, "");
    return GrammarBuilder().Text(Expression(root));
}

} // namespace morpheus::grammar
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed` (13 test cases).

- [ ] **Step 8: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 348`.

- [ ] **Step 9: Commit**

```bash
git add src/grammar/CMakeLists.txt src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/TestSchemas.h tests/grammar/SchemaTests.cpp
git commit -m "Add FromSchema: GBNF for a subset of JSON Schema" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

