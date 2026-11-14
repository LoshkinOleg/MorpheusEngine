#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;
using namespace nlohmann::literals;

namespace {

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

// The message of the DataError Validate throws for `schema`.
std::string ValidateSchemaError(const nlohmann::json& schema)
{
    try {
        grammar::Validate(nlohmann::json(), schema);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

} // namespace

TEST_CASE("Validate accepts numbers within their optional bounds", "[schema]")
{
    const nlohmann::json any = R"({"type": "number"})"_json;
    CHECK(grammar::Validate(nlohmann::json(3), any).empty());
    CHECK(grammar::Validate(nlohmann::json(-2.5), any).empty());
    CHECK(grammar::Validate(nlohmann::json::parse("18446744073709551615"), any).empty());
    const grammar::ValidationError text = OnlyError(nlohmann::json("3"), any);
    CHECK(text.path.empty());
    CHECK(text.message == "expected a number, got '3'");
    CHECK(OnlyError(nlohmann::json(true), any).message == "expected a number, got true"); // A bool is not a number.

    const nlohmann::json hours = R"({"type": "number", "minimum": 0, "maximum": 23.5})"_json;
    CHECK(grammar::Validate(nlohmann::json(0), hours).empty());
    CHECK(grammar::Validate(nlohmann::json(23.5), hours).empty());
    CHECK(OnlyError(nlohmann::json(24), hours).message == "expected a number from 0 to 23.5, got 24");
    CHECK(OnlyError(nlohmann::json(-0.25), hours).message == "expected a number from 0 to 23.5, got -0.25");
    CHECK(OnlyError(nlohmann::json(1), R"({"type": "number", "minimum": 1.5})"_json).message ==
          "expected a number of at least 1.5, got 1");
    CHECK(OnlyError(nlohmann::json(0), R"({"type": "number", "maximum": -1})"_json).message ==
          "expected a number of at most -1, got 0");

    // A number's bounds are numbers, in order, and nothing else applies to it.
    CHECK(ValidateSchemaError(R"({"type": "number", "minimum": "0"})"_json) ==
          "schema at /minimum: 'minimum' must be a number");
    CHECK(ValidateSchemaError(R"({"type": "number", "minimum": 2, "maximum": 1})"_json) ==
          "schema at the root: 'minimum' 2 is above 'maximum' 1");
    CHECK(ValidateSchemaError(R"({"type": "number", "maxLength": 3})"_json) ==
          "schema at /maxLength: 'maxLength' does not apply to a number");
}

TEST_CASE("Validate accepts booleans", "[schema]")
{
    const nlohmann::json schema = R"({"type": "boolean"})"_json;
    CHECK(grammar::Validate(nlohmann::json(true), schema).empty());
    CHECK(grammar::Validate(nlohmann::json(false), schema).empty());
    CHECK(OnlyError(nlohmann::json(0), schema).message == "expected a boolean, got 0");
    CHECK(OnlyError(nlohmann::json("true"), schema).message == "expected a boolean, got 'true'");
    CHECK(ValidateSchemaError(R"({"type": "boolean", "minimum": 0})"_json) ==
          "schema at /minimum: 'minimum' does not apply to a boolean");
}

TEST_CASE("Validate treats properties outside required as optional", "[schema]")
{
    const nlohmann::json schema = R"({"type": "object", "required": ["id"], "properties": {
        "id": {"type": "string", "maxLength": 8},
        "bio": {"type": "string"},
        "age": {"type": "integer", "minimum": 0, "maximum": 200}}})"_json;
    CHECK(grammar::Validate(R"({"id": "a"})"_json, schema).empty());
    CHECK(grammar::Validate(R"({"id": "a", "bio": "b", "age": 3})"_json, schema).empty());

    // A present optional key is checked; a missing required one is an error; extra keys are not allowed.
    const grammar::ValidationError age = OnlyError(R"({"id": "a", "age": -1})"_json, schema);
    CHECK(age.path == "/age");
    CHECK(age.message == "expected an integer from 0 to 200, got -1");
    const grammar::ValidationError missing = OnlyError(R"({"bio": "b"})"_json, schema);
    CHECK(missing.path == "/id");
    CHECK(missing.message == "missing key 'id'");
    // The keys are listed required first, then the optional ones by name.
    const grammar::ValidationError extra = OnlyError(R"({"id": "a", "rank": 1})"_json, schema);
    CHECK(extra.path == "/rank");
    CHECK(extra.message == "unexpected key 'rank'; the keys are 'id', 'age', 'bio'");
    CHECK(OnlyError(nlohmann::json(3), schema).message == "expected an object with the keys 'id'");
    const nlohmann::json allOptional = R"({"type": "object", "required": [], "properties": {"a": {"type": "boolean"}}})"_json;
    CHECK(grammar::Validate(nlohmann::json::object(), allOptional).empty());
    CHECK(OnlyError(nlohmann::json(3), allOptional).message == "expected an object");

    // A oneOf branch still needs its discriminator, the first key of `required`.
    CHECK(ValidateSchemaError(R"({"oneOf": [{"type": "object", "properties": {"action": {"const": "x"}}, "required": []}]})"_json) ==
          "schema at /oneOf/0/required: a 'oneOf' branch needs a discriminator, the first key of 'required'");
}

TEST_CASE("Validate lets an open object hold extra keys, unchecked", "[schema]")
{
    const nlohmann::json schema = R"({"type": "object", "required": ["id"], "additionalProperties": true,
        "properties": {"id": {"type": "string", "maxLength": 8}}})"_json;
    CHECK(grammar::Validate(R"({"id": "a", "gender": "female", "equipment": {"body": "robes"}})"_json, schema).empty());
    const grammar::ValidationError id = OnlyError(R"({"id": 7, "extra": 1})"_json, schema);
    CHECK(id.path == "/id");
    CHECK(id.message == "expected a string of at most 8 characters, got 7");

    const nlohmann::json anything = R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json;
    CHECK(grammar::Validate(R"({"anything": [1, 2]})"_json, anything).empty());
    CHECK(OnlyError(nlohmann::json::array(), anything).message == "expected an object");
    CHECK(ValidateSchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": "yes"})"_json) ==
          "schema at /additionalProperties: 'additionalProperties' must be true or false");
}

TEST_CASE("Validate accepts arrays without maxItems and strings without maxLength", "[schema]")
{
    const nlohmann::json strings = R"({"type": "array", "items": {"type": "string"}})"_json;
    CHECK(grammar::Validate(nlohmann::json::array(), strings).empty());
    std::vector<std::string> many(1200, "x");
    many.push_back(std::string(2500, 'y'));
    CHECK(grammar::Validate(nlohmann::json(many), strings).empty());
    CHECK(OnlyError(nlohmann::json("x"), strings).message == "expected an array, got 'x'");
    const grammar::ValidationError item = OnlyError(R"(["a", 2])"_json, strings);
    CHECK(item.path == "/1");
    CHECK(item.message == "expected a string, got 2");

    const nlohmann::json atLeastTwo = R"({"type": "array", "items": {"const": 1}, "minItems": 2})"_json;
    CHECK(OnlyError(R"([1])"_json, atLeastTwo).message == "has 1 items; expected at least 2");
    CHECK(OnlyError(nlohmann::json("x"), atLeastTwo).message == "expected an array of at least 2 items, got 'x'");
    const nlohmann::json named = R"({"type": "string", "minLength": 1})"_json;
    CHECK(OnlyError(nlohmann::json(""), named).message == "shorter than 1 characters");
    CHECK(OnlyError(nlohmann::json(3), named).message == "expected a string of at least 1 characters, got 3");

    // llama.cpp's limits bound grammars only: a schema that is only validated may go past them.
    CHECK(grammar::Validate(nlohmann::json(std::string(2500, 'a')), R"({"type": "string", "maxLength": 3000})"_json).empty());
    CHECK(grammar::Validate(nlohmann::json(std::vector<int>(1500, 1)),
                            R"({"type": "array", "items": {"const": 1}, "maxItems": 5000})"_json)
              .empty());
}

TEST_CASE("FromSchema still refuses every schema only Validate accepts", "[schema]")
{
    const std::vector<std::string> validationOnly = {
        R"({"type": "number"})",
        R"({"type": "number", "minimum": 0, "maximum": 1})",
        R"({"type": "boolean"})",
        R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})",
        R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})",
        R"({"type": "array", "items": {"const": 1}})",
        R"({"type": "string"})",
        R"({"type": "string", "maxLength": 2000})",
        R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})",
    };
    for (const std::string& text : validationOnly) {
        INFO(text);
        const nlohmann::json schema = nlohmann::json::parse(text);
        CHECK_THROWS_AS(grammar::FromSchema(schema), core::DataError);
        CHECK_NOTHROW(grammar::Validate(nlohmann::json(), schema));
    }
    // Today's messages, word for word.
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "boolean"})"_json), "schema at /type: the type 'boolean' is not supported");
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "string"})"_json), "schema at the root: a string needs 'maxLength'");
    CHECK_THROWS_WITH(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json),
                      "schema at /additionalProperties: 'additionalProperties' may only be false: extra keys are never allowed");
}

TEST_CASE("CheckValidationSchema accepts the extended subset and throws Validate's schema errors", "[schema]")
{
    CHECK_NOTHROW(grammar::CheckValidationSchema(test::CatalogSchema()));
    CHECK_NOTHROW(grammar::CheckValidationSchema(R"({"type": "object", "required": [], "additionalProperties": true,
        "properties": {"n": {"type": "number"}, "b": {"type": "boolean"}, "s": {"type": "string"}}})"_json));
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "string", "pattern": "a+"})"_json),
                      "schema at /pattern: the keyword 'pattern' is not supported");
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "null"})"_json),
                      "schema at /type: the type 'null' is not supported");
    CHECK_THROWS_WITH(grammar::CheckValidationSchema(R"({"type": "integer", "minimum": 0})"_json),
                      "schema at the root: an integer needs 'minimum' and 'maximum'");
}
