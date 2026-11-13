#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
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

// The grammar FromSchema must give for an array of minItems to maxItems items whose expression is `itemBody`: the
// item is the rule `items-0`.
std::string ArrayGrammarOf(const std::string& itemBody, int minItems, int maxItems)
{
    grammar::GrammarBuilder builder;
    const std::string items = builder.Rule("items-0", itemBody);
    return builder.Text(grammar::ListOf(items, minItems, maxItems));
}

// The message of the DataError FromSchema throws for the schema `text`.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    std::optional<std::string> message;
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        message = error.what();
    }
    INFO("FromSchema accepted the schema " << text);
    REQUIRE(message.has_value());
    return *message;
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
          ArrayGrammarOf(grammar::Literal("1"), 0, 2));
    CHECK(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "minItems": 1, "maxItems": 3})"_json) ==
          ArrayGrammarOf(grammar::Literal("1"), 1, 3));
    // Each array's items are a rule of their own, numbered in the order the rules are written: inner arrays first.
    CHECK(grammar::FromSchema(
              R"({"type": "array", "maxItems": 3, "items": {"type": "array", "items": {"const": 1}, "maxItems": 2}})"_json) ==
          R"gbnf(root ::= ("[]" | "[" items-1 ("," items-1){0,2} "]")
items-0 ::= "1"
items-1 ::= ("[]" | "[" items-0 ("," items-0){0,1} "]")
)gbnf");
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
        with(R"("version":2)", R"("version":"2")"),                       // a string where the const is an integer
        with(R"("codes":[])", R"("codes":["3"])"),                        // a string where the enum holds integers
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
    CHECK(SchemaError(R"({"type": "string", "maxLength": 3, "items": {"const": 1}})") ==
          "schema at /items: 'items' does not apply to a string");
    // A string repeats `char` up to maxLength times, which llama.cpp allows up to 1999.
    CHECK_NOTHROW(grammar::FromSchema(R"({"type": "string", "maxLength": 1999})"_json));
    CHECK(SchemaError(R"({"type": "string", "maxLength": 2000})") ==
          "schema at /maxLength: 'maxLength' 2000 is above 1999, the most a grammar llama.cpp loads can hold");
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
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 3, "maxLength": 3})") ==
          "schema at /maxLength: 'maxLength' does not apply to an array");
    // An array repeats the items after its first up to maxItems - 1 times, which llama.cpp allows up to 1000 items.
    CHECK_NOTHROW(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "maxItems": 1000})"_json));
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})") ==
          "schema at /maxItems: 'maxItems' 1001 is above 1000, the most a grammar llama.cpp loads can hold");
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
    CHECK(SchemaError(R"({"oneOf": [)" + x + R"(], "maxItems": 3})") ==
          "schema at /maxItems: 'maxItems' does not apply to a oneOf");
}

TEST_CASE("FromSchema names the pointer of a mistake deep inside the schema", "[schema]")
{
    // The catalog's attack target with no maxLength: the spec's example pointer.
    nlohmann::json schema = test::CatalogSchema();
    schema["oneOf"][3]["properties"]["params"]["properties"]["target"] = nlohmann::json::parse(R"({"type": "string"})");
    CHECK(SchemaError(schema.dump()) ==
          "schema at /oneOf/3/properties/params/properties/target: a string needs 'maxLength'");
}
