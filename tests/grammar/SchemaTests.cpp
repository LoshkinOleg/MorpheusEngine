#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "grammar/Check.h"
#include "grammar/Gbnf.h"
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

// The message of the DataError FromSchema throws for the schema `text`, after checking that Validate throws the same.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    std::string fromSchema;
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        fromSchema = error.what();
    }
    std::string validate;
    try {
        grammar::Validate(nlohmann::json::object(), schema);
    } catch (const core::DataError& error) {
        validate = error.what();
    }
    INFO("schema: " << text);
    REQUIRE_FALSE(fromSchema.empty());
    CHECK(validate == fromSchema);
    return fromSchema;
}

// The message of the DataError FromSchema throws for the schema `text`, after checking that Validate accepts it: the
// schema is in the extended subset of schemas that are only validated.
std::string GrammarOnlyError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    std::string fromSchema;
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        fromSchema = error.what();
    }
    INFO("schema: " << text);
    REQUIRE_FALSE(fromSchema.empty());
    CHECK_NOTHROW(grammar::Validate(nlohmann::json::object(), schema));
    return fromSchema;
}

// The one error Validate finds in `value` against `schema`.
grammar::ValidationError OnlyError(const nlohmann::json& value, const nlohmann::json& schema)
{
    const std::vector<grammar::ValidationError> errors = grammar::Validate(value, schema);
    INFO("value: " << value.dump());
    for (const grammar::ValidationError& error : errors) {
        UNSCOPED_INFO(error.path << ": " << error.message);
    }
    REQUIRE(errors.size() == 1);
    return errors[0];
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
    const std::string matcher = grammar::FromSchema(test::CatalogSchema());
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
        CHECK(grammar::Check(matcher, text).empty());
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
        CHECK_FALSE(grammar::Check(matcher, text).empty());
    }
}

TEST_CASE("The grammar of every keyword accepts a valid value and refuses each broken one", "[schema]")
{
    const std::string matcher = grammar::FromSchema(test::EveryKeywordSchema());
    const std::string base =
        R"({"name":"ab","count":0,"big":5,"tags":["red"],"codes":[],"version":2,"pair":[{"x":1,"label":"point"},{"x":2,"label":"point"}],"empty":{},"mode":{"state":"off"}})";
    // `base` with its first `from` replaced by `to`.
    auto with = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    };
    CHECK(grammar::Check(matcher, base).empty());
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
        CHECK(grammar::Check(matcher, text).empty());
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
        CHECK_FALSE(grammar::Check(matcher, text).empty());
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
    CHECK(GrammarOnlyError(R"({"type": "boolean"})") == "schema at /type: the type 'boolean' is not supported");
    CHECK(GrammarOnlyError(R"({"type": "number"})") == "schema at /type: the type 'number' is not supported");
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
    CHECK(GrammarOnlyError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})") ==
          "schema at /properties/a: 'a' must be in 'required': optional keys are not supported");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": ["a"]})") ==
          "schema at /required/0: 'a' is required but not in 'properties'");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": ["a", "a"]})") ==
          "schema at /required/1: 'a' is listed twice");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": [1]})") ==
          "schema at /required/0: 'required' entries must be strings");
    CHECK(GrammarOnlyError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})") ==
          "schema at /additionalProperties: 'additionalProperties' may only be false: extra keys are never allowed");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": [], "minItems": 1})") ==
          "schema at /minItems: 'minItems' does not apply to an object");
    // A key with "/" and "~" is escaped in the pointer as RFC 6901 says.
    CHECK(GrammarOnlyError(R"({"type": "object", "properties": {"a/b~c": {"type": "string"}}, "required": ["a/b~c"]})") ==
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
    CHECK(GrammarOnlyError(R"({"type": "string"})") == "schema at the root: a string needs 'maxLength'");
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
    CHECK(GrammarOnlyError(R"({"type": "string", "maxLength": 2000})") ==
          "schema at /maxLength: 'maxLength' 2000 is above 1999, the most a grammar llama.cpp loads can hold");
}

TEST_CASE("FromSchema refuses arrays outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "array", "maxItems": 3})") == "schema at the root: an array needs 'items'");
    CHECK(GrammarOnlyError(R"({"type": "array", "items": {"const": 1}})") == "schema at the root: an array needs 'maxItems'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 0})") ==
          "schema at /maxItems: 'maxItems' must be at least 1");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "minItems": 4, "maxItems": 3})") ==
          "schema at the root: 'minItems' 4 is above 'maxItems' 3");
    CHECK(GrammarOnlyError(R"({"type": "array", "items": {"type": "string"}, "maxItems": 3})") ==
          "schema at /items: a string needs 'maxLength'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 3, "maxLength": 3})") ==
          "schema at /maxLength: 'maxLength' does not apply to an array");
    // An array repeats the items after its first up to maxItems - 1 times, which llama.cpp allows up to 1000 items.
    CHECK_NOTHROW(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "maxItems": 1000})"_json));
    CHECK(GrammarOnlyError(R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})") ==
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
    CHECK(GrammarOnlyError(schema.dump()) ==
          "schema at /oneOf/3/properties/params/properties/target: a string needs 'maxLength'");
}

TEST_CASE("Validate accepts every action the catalog allows", "[schema]")
{
    const nlohmann::json schema = test::CatalogSchema();
    const std::vector<std::string> valid = {
        R"({"action":"none","params":{}})",
        R"({"action":"follow","params":{}})",
        R"({"action":"wait","params":{}})",
        R"({"action":"attack","params":{"target":"0001A6A4"}})",
        R"({"action":"give_item","params":{"item":"Iron Sword"}})",
        R"({"action":"give_item","params":{"item":")" + LongestItem() + R"("}})",
        R"({"action":"end_conversation","params":{}})",
    };
    for (const std::string& text : valid) {
        INFO(text);
        CHECK(grammar::Validate(nlohmann::json::parse(text), schema).empty());
    }
}

TEST_CASE("Validate reports each mistake in a catalog action at its path", "[schema]")
{
    const nlohmann::json schema = test::CatalogSchema();
    const std::string actions = R"("none", "follow", "wait", "attack", "give_item", "end_conversation")";

    // An unknown action: one error, at the discriminator.
    const grammar::ValidationError dance = OnlyError(R"({"action":"dance","params":{}})"_json, schema);
    CHECK(dance.path == "/action");
    CHECK(dance.message == "unknown action 'dance'; expected one of: " + actions);

    // An actor who is not present, and a name where an id belongs.
    const grammar::ValidationError absent = OnlyError(R"({"action":"attack","params":{"target":"0001C3D2"}})"_json, schema);
    CHECK(absent.path == "/params/target");
    CHECK(absent.message == R"('0001C3D2' is not one of: "0001A6A4", "0001B07C")");
    CHECK(OnlyError(R"({"action":"attack","params":{"target":"Saadia"}})"_json, schema).message ==
          R"('Saadia' is not one of: "0001A6A4", "0001B07C")");

    // A missing argument.
    const grammar::ValidationError missing = OnlyError(R"({"action":"attack","params":{}})"_json, schema);
    CHECK(missing.path == "/params/target");
    CHECK(missing.message == "missing key 'target'");

    // An item over 48 characters.
    const grammar::ValidationError item = OnlyError(
        nlohmann::json::parse(R"({"action":"give_item","params":{"item":")" + LongestItem() + R"(a"}})"), schema);
    CHECK(item.path == "/params/item");
    CHECK(item.message == "longer than 48 characters");

    // An extra key, the host's measured Qwen failure: "intent" added to wait, at the top or among the arguments.
    const grammar::ValidationError intent = OnlyError(R"({"action":"wait","intent":"rest","params":{}})"_json, schema);
    CHECK(intent.path == "/intent");
    CHECK(intent.message == "unexpected key 'intent'; the keys are 'action', 'params'");
    const grammar::ValidationError argument = OnlyError(R"({"action":"wait","params":{"intent":"rest"}})"_json, schema);
    CHECK(argument.path == "/params/intent");
    CHECK(argument.message == "unexpected key 'intent'; the object must be empty");

    // No discriminator, or no object at all.
    const grammar::ValidationError noAction = OnlyError(R"({"params":{}})"_json, schema);
    CHECK(noAction.path == "/action");
    CHECK(noAction.message == "missing key 'action'");
    const grammar::ValidationError notObject = OnlyError(R"("wait")"_json, schema);
    CHECK(notObject.path == "");
    CHECK(notObject.message == "expected an object whose 'action' is one of: " + actions);
}

TEST_CASE("Validate wants a JSON integer where the schema says integer", "[schema]")
{
    const nlohmann::json schema =
        R"({"type": "object", "properties": {"count": {"type": "integer", "minimum": 1, "maximum": 5}}, "required": ["count"]})"_json;
    CHECK(grammar::Validate(R"({"count": 5})"_json, schema).empty());
    const grammar::ValidationError fraction = OnlyError(R"({"count": 2.5})"_json, schema);
    CHECK(fraction.path == "/count");
    CHECK(fraction.message == "expected an integer from 1 to 5, got 2.5");
    // A float with an integral value is still a float.
    CHECK(OnlyError(R"({"count": 2.0})"_json, schema).message == "expected an integer from 1 to 5, got 2.0");
    CHECK(OnlyError(R"({"count": "2"})"_json, schema).message == "expected an integer from 1 to 5, got '2'");
    CHECK(OnlyError(R"({"count": true})"_json, schema).message == "expected an integer from 1 to 5, got true");
    CHECK(OnlyError(R"({"count": 6})"_json, schema).message == "expected an integer from 1 to 5, got 6");
}

TEST_CASE("Validate reports every error, in document order", "[schema]")
{
    const nlohmann::json schema = R"({
        "type": "object",
        "properties": {
            "name": {"type": "string", "maxLength": 3},
            "tags": {"type": "array", "items": {"enum": ["a", "b"]}, "maxItems": 2},
            "level": {"type": "integer", "minimum": 0, "maximum": 3}
        },
        "required": ["name", "tags", "level"]
    })"_json;
    const nlohmann::json value = R"({"level": 7, "zz": true, "tags": ["a", "c", "d"], "extra": 1, "name": "long name"})"_json;
    const std::vector<grammar::ValidationError> errors = grammar::Validate(value, schema);
    // The schema's keys in its order (the order the grammar writes them), then the keys it does not allow.
    REQUIRE(errors.size() == 7);
    CHECK(errors[0].path == "/name");
    CHECK(errors[0].message == "longer than 3 characters");
    CHECK(errors[1].path == "/tags");
    CHECK(errors[1].message == "has 3 items; expected 0 to 2");
    CHECK(errors[2].path == "/tags/1");
    CHECK(errors[2].message == R"('c' is not one of: "a", "b")");
    CHECK(errors[3].path == "/tags/2");
    CHECK(errors[3].message == R"('d' is not one of: "a", "b")");
    CHECK(errors[4].path == "/level");
    CHECK(errors[4].message == "expected an integer from 0 to 3, got 7");
    CHECK(errors[5].path == "/extra");
    CHECK(errors[5].message == "unexpected key 'extra'; the keys are 'name', 'tags', 'level'");
    CHECK(errors[6].path == "/zz");
    CHECK(errors[6].message == "unexpected key 'zz'; the keys are 'name', 'tags', 'level'");
}

TEST_CASE("Validate checks every keyword of the subset", "[schema]")
{
    const nlohmann::json schema = test::EveryKeywordSchema();
    const nlohmann::json base =
        R"({"name":"ab","count":0,"big":5,"tags":["red"],"codes":[],"version":2,"pair":[{"x":1,"label":"point"},{"x":2,"label":"point"}],"empty":{},"mode":{"state":"off"}})"_json;
    CHECK(grammar::Validate(base, schema).empty());
    // `base` with `key` set to `replacement` gives exactly one error, at `path`, saying `message`.
    auto expectOne = [&](const char* key, const nlohmann::json& replacement, const std::string& path,
                         const std::string& message) {
        nlohmann::json value = base;
        value[key] = replacement;
        INFO(key << " = " << replacement.dump());
        const grammar::ValidationError found = OnlyError(value, schema);
        CHECK(found.path == path);
        CHECK(found.message == message);
    };
    expectOne("version", 3, "/version", "expected 2, got 3");
    expectOne("count", 121, "/count", "expected an integer from -20 to 120, got 121");
    expectOne("big", 1.5, "/big", "expected an integer from -9223372036854775808 to 9223372036854775807, got 1.5");
    expectOne("name", "a", "/name", "shorter than 2 characters");
    expectOne("name", "abcdefg", "/name", "longer than 6 characters");
    expectOne("name", 5, "/name", "expected a string of 2 to 6 characters, got 5");
    expectOne("tags", nlohmann::json::array(), "/tags", "has 0 items; expected 1 to 3");
    expectOne("tags", R"(["pink"])"_json, "/tags/0", R"('pink' is not one of: "red", "green", "blue")");
    expectOne("codes", R"([3.0])"_json, "/codes/0", "3.0 is not one of: 3, -7, 120");
    expectOne("codes", "3", "/codes", "expected an array of 0 to 2 items, got '3'");
    expectOne("pair", R"([{"x":1,"label":"point"},{"x":10,"label":"point"}])"_json, "/pair/1/x",
              "expected an integer from 0 to 9, got 10");
    expectOne("empty", R"({"a":1})"_json, "/empty/a", "unexpected key 'a'; the object must be empty");
    expectOne("empty", nlohmann::json::array(), "/empty", "expected an empty object");
    expectOne("mode", R"({"state":"on"})"_json, "/mode/level", "missing key 'level'");
    expectOne("mode", R"({"state":"dim"})"_json, "/mode/state", R"(unknown state 'dim'; expected one of: "on", "off")");

    // A key with "/" and "~" is escaped in the path as RFC 6901 says.
    const nlohmann::json escaped = R"({"type": "object", "properties": {"a/b~c": {"const": 1}}, "required": ["a/b~c"]})"_json;
    const grammar::ValidationError odd = OnlyError(R"({"a/b~c": 2})"_json, escaped);
    CHECK(odd.path == "/a~1b~0c");
    CHECK(odd.message == "expected 1, got 2");
}

TEST_CASE("Validate throws the schema's DataError and never one for the value", "[schema]")
{
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "null"})"_json), core::DataError);
    // A value of any shape is data: errors, not exceptions.
    const nlohmann::json schema = test::CatalogSchema();
    const std::vector<nlohmann::json> values = {nlohmann::json(), nlohmann::json(1.5), nlohmann::json::array({1, 2}),
                                                R"({"action": null})"_json, R"({"action": "attack", "params": []})"_json};
    for (const nlohmann::json& value : values) {
        INFO(value.dump());
        std::vector<grammar::ValidationError> errors;
        CHECK_NOTHROW(errors = grammar::Validate(value, schema));
        CHECK_FALSE(errors.empty());
    }
    // One of them in full: a null where the discriminator belongs.
    const std::vector<grammar::ValidationError> nulled = grammar::Validate(R"({"action": null})"_json, schema);
    REQUIRE(nulled.size() == 1);
    CHECK(nulled[0].path == "/action");
    CHECK(nulled[0].message.starts_with("unknown action null; expected one of: "));
}

TEST_CASE("Validate tells a negative integer from an unsigned one that nlohmann would equate", "[schema]")
{
    // nlohmann's == casts an unsigned to int64 against a signed: -1 == 18446744073709551615. The grammar's Literal
    // compares text, so Validate must too.
    const nlohmann::json bigUnsigned = nlohmann::json::parse("18446744073709551615");
    const nlohmann::json minusOne = nlohmann::json::parse("-1");
    REQUIRE(minusOne == bigUnsigned);
    const nlohmann::json inEnum = R"({"enum": [-1, 5]})"_json;
    CHECK(grammar::Validate(minusOne, inEnum).empty());
    CHECK(grammar::Validate(nlohmann::json(5), inEnum).empty());
    CHECK(grammar::Validate(bigUnsigned, inEnum).size() == 1);
    const nlohmann::json minConst = nlohmann::json::parse(R"({"const": -9223372036854775808})");
    CHECK(grammar::Validate(nlohmann::json::parse("-9223372036854775808"), minConst).empty());
    CHECK(grammar::Validate(nlohmann::json::parse("9223372036854775808"), minConst).size() == 1);
    // A uint64 above the long long range is a legal enum value (Literal writes its text): not a duplicate of -1.
    const nlohmann::json both = nlohmann::json::parse(R"({"enum": [-1, 18446744073709551615]})");
    CHECK_NOTHROW(grammar::FromSchema(both));
    CHECK(grammar::Validate(bigUnsigned, both).empty());
    CHECK(grammar::Validate(minusOne, both).empty());
    // An integer node: huge values are errors, not throws.
    const nlohmann::json range = R"({"type": "integer", "minimum": 0, "maximum": 5})"_json;
    CHECK(grammar::Validate(bigUnsigned, range).size() == 1);
    CHECK(grammar::Validate(nlohmann::json::parse("9223372036854775808"), range).size() == 1);
}
