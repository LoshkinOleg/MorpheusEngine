### Task 1: The extended `grammar::Validate` subset

`Validate` learns the keywords a schema that is only validated may use: numbers with optional bounds, booleans, properties outside `required` (optional), `additionalProperties: true` (extra keys allowed and unchecked), arrays without `maxItems` and strings without `maxLength`. `FromSchema` refuses every one of them with today's messages, because it must bound the grammar. `CheckValidationSchema` checks a schema against the extended subset without a value (`HostedWorld` checks the observation schema at open with it).

**Files:**
- Modify: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`
- Create: `tests/grammar/ValidateExtendedTests.cpp`
- Modify: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::vector<grammar::ValidationError> grammar::Validate(const nlohmann::json& value, const nlohmann::json& schema)` (same signature, extended subset); `void grammar::CheckValidationSchema(const nlohmann::json& schema)` in `grammar/Schema.h` (throws the `DataError` `Validate` would throw for `schema`, `"schema at <pointer>: <problem>"`). New value messages: `"expected a number, got X"`, `"expected a number from A to B, got X"`, `"expected a number of at least A, got X"`, `"expected a number of at most B, got X"`, `"expected a boolean, got X"`, `"expected an object"`, `"expected a string, got X"`, `"expected a string of at least N characters, got X"`, `"expected an array, got X"`, `"expected an array of at least N items, got X"`, `"has N items; expected at least M"`. New schema messages: `"'minimum' must be a number"`, `"'additionalProperties' must be true or false"`, `"'X' does not apply to a number"` / `"to a boolean"`.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/ValidateExtendedTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

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
```

In `tests/grammar/SchemaTests.cpp`, after the `SchemaError` helper (just before `// The one error Validate finds in `value` against `schema`.`), add:

```cpp
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
```

Then, in the same file, replace `SchemaError(` with `GrammarOnlyError(` in exactly these eleven calls (each expected message stays as it is):

1. `SchemaError(R"({"type": "boolean"})")` (test "FromSchema refuses what is not a schema of the subset, naming the pointer")
2. `SchemaError(R"({"type": "number"})")` (same test)
3. `SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})")` (test "FromSchema refuses objects outside the subset")
4. `SchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})")` (same test)
5. `SchemaError(R"({"type": "object", "properties": {"a/b~c": {"type": "string"}}, "required": ["a/b~c"]})")` (same test)
6. `SchemaError(R"({"type": "string"})")` (test "FromSchema refuses strings outside the subset")
7. `SchemaError(R"({"type": "string", "maxLength": 2000})")` (same test)
8. `SchemaError(R"({"type": "array", "items": {"const": 1}})")` (test "FromSchema refuses arrays outside the subset")
9. `SchemaError(R"({"type": "array", "items": {"type": "string"}, "maxItems": 3})")` (same test)
10. `SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 1001})")` (same test)
11. `SchemaError(schema.dump())` (test "FromSchema names the pointer of a mistake deep inside the schema")

And in the test "Validate throws the schema's DataError and never one for the value", replace

```cpp
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "boolean"})"_json), core::DataError);
```

with

```cpp
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "null"})"_json), core::DataError);
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `'CheckValidationSchema': is not a member of 'morpheus::grammar'` in `tests/grammar/ValidateExtendedTests.cpp`.

- [ ] **Step 3: Write the extended subset**

In `src/grammar/Schema.h`, replace the `Validate` comment and declaration (from `// Checks `value` against `schema`: exactly what FromSchema's grammar allows.` to `std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);`) with:

```cpp
// Checks `value` against `schema`. The schema may use everything FromSchema supports, plus what only a validated
// schema may (the extended subset, for schemas such as a host's observation schema that never become a grammar):
//   {"type": "number"}                    any JSON number (not a bool), with optional "minimum" and "maximum" (any
//                                         numbers, a <= b), both included
//   {"type": "boolean"}
//   object properties not in "required"   optional: checked when present; they follow the required keys, by name
//   "additionalProperties": true          extra keys are allowed and not checked
//   {"type": "array"} without "maxItems"  optional "minItems"; "maxItems", when given, at least 1
//   {"type": "string"} without "maxLength"  optional "minLength"
// and llama.cpp's limits on "maxLength" and "maxItems" do not apply. What FromSchema's grammar allows is checked as
// it allows it: types (an integer is a JSON integer, not a float), enum and const, integer ranges, string lengths in
// characters (core::Utf8Length), required keys present and no extra keys unless the object is open, array sizes and
// each item. For a oneOf the discriminator's value picks the branch: a missing discriminator, or a value naming no
// branch, is one error at the discriminator's path; otherwise the errors are the chosen branch's, with full paths.
// Every error is reported, in document order: an object's keys in the schema's order (required keys in `required`
// order, then optional keys by name), each with its nested errors, then the keys the schema does not allow. An empty
// result means the value is valid. Never throws on the value: a mistake in the value is data. A schema outside the
// extended subset throws a DataError of FromSchema's form, "schema at <pointer>: <problem>".
// Two limits of a parsed value: the grammar's shared `char` rule admits a lone surrogate escape in the model's text,
// which strict JSON parsers such as nlohmann's refuse, so a host must treat a parse failure of the model's text as a
// model mistake before it calls Validate; and key order is enforced by the grammar, not by Validate, because a parsed
// value has lost it.
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);

// Checks that `schema` is in Validate's extended subset, without a value: throws the DataError Validate would throw
// for it, and returns when Validate accepts it. A host's validation-only schema is checked with it when it arrives,
// so an unsupported keyword fails then rather than at the first value.
void CheckValidationSchema(const nlohmann::json& schema);
```

Replace the whole of `src/grammar/Schema.cpp` with:

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
#include "core/Text.h"
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
// A string's or an array's count with no upper limit: only a schema that is validated, never written as a grammar,
// has one.
constexpr int UNBOUNDED = -1;
// A number's absent bounds.
constexpr double NO_MINIMUM = -std::numeric_limits<double>::infinity();
constexpr double NO_MAXIMUM = std::numeric_limits<double>::infinity();

// What a schema is read for. A grammar must be bounded, so FromSchema reads the subset; Validate and
// CheckValidationSchema read the extended subset of schemas that are only validated.
enum class Use { Grammar, Validation };

// A schema checked against the subset, as FromSchema and Validate read it.
struct Node {
    enum class Kind { Object, Enum, Const, Integer, Number, Boolean, String, Array, OneOf };

    Kind kind = Kind::Object;
    // Object: its keys, the required ones in `required` order, then the optional ones (Validation only) by name.
    std::vector<std::string> keys = {};
    std::size_t requiredKeys = 0;            // Object: how many of `keys`, from the first, are required.
    bool open = false;                       // Object: "additionalProperties": true (Validation only).
    std::vector<Node> children = {};         // Object: one value schema per key; Array: the item schema;
                                             // OneOf: the branches, each an Object whose first key is the discriminator.
    std::vector<nlohmann::json> values = {}; // Enum: the allowed values (all strings or all integers); Const: the one.
    long long minimum = 0;                   // Integer: the bounds, both included.
    long long maximum = 0;
    double lowest = NO_MINIMUM;              // Number (Validation only): the bounds, both included.
    double highest = NO_MAXIMUM;
    int minCount = 0;                        // String: characters; Array: items.
    int maxCount = 0;                        // UNBOUNDED when there is no upper limit (Validation only).
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

// Equal as the grammar sees it: a string to a string, an integer to an integer (1.0 is not 1). Integers are compared by
// their text, as the grammar's Literal writes them: nlohmann's == would equate -1 with 18446744073709551615.
bool SameValue(const nlohmann::json& a, const nlohmann::json& b)
{
    if (a.is_string() && b.is_string()) {
        return a == b;
    }
    return a.is_number_integer() && b.is_number_integer() && a.dump() == b.dump();
}

// Reads `schema`, found at `pointer` inside the whole schema, into a Node, checking it against the subset `use` reads.
// Throws DataError naming the pointer of the mistake. Recursive: the schemas of an object's properties, an array's
// items and a oneOf's branches follow the same rules.
Node Compile(const nlohmann::json& schema, const std::string& pointer, Use use)
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
    // A count keyword above `most`, the largest a grammar llama.cpp loads can hold (see GBNF_MAX_STRING_CHARS).
    auto beyondLlamaLimit = [&](const char* keyword, long long count, long long most) {
        return error(pointer + "/" + keyword,
                     std::format("'{}' {} is above {}, the most a grammar llama.cpp loads can hold", keyword, count, most));
    };
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();
    constexpr long long MOST_COUNT = std::numeric_limits<int>::max();
    const bool grammarUse = use == Use::Grammar;

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
            Node compiled = Compile(branch, branchAt, use);
            // The discriminator: the first key of the branch's `required`, the same key in every branch, and a const
            // string in each that no other branch has.
            if (compiled.requiredKeys == 0) {
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
                if (SameValue(values[j], value)) {
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

    // A number and a boolean are only validated: a grammar of them is not part of the subset.
    if (typeName == "number" && !grammarUse) {
        allowOnly("a number", {"type", "minimum", "maximum"});
        // A bound: absent, or any JSON number (a bool is not one).
        auto bound = [&](const char* keyword, double absent) {
            if (!schema.contains(keyword)) {
                return absent;
            }
            const nlohmann::json& value = schema.at(keyword);
            if (!value.is_number()) {
                throw error(pointer + "/" + keyword, std::format("'{}' must be a number", keyword));
            }
            return value.get<double>();
        };
        node.kind = Node::Kind::Number;
        node.lowest = bound("minimum", NO_MINIMUM);
        node.highest = bound("maximum", NO_MAXIMUM);
        if (node.lowest > node.highest) {
            throw error(pointer, std::format("'minimum' {} is above 'maximum' {}", node.lowest, node.highest));
        }
        return node;
    }
    if (typeName == "boolean" && !grammarUse) {
        allowOnly("a boolean", {"type"});
        node.kind = Node::Kind::Boolean;
        return node;
    }

    if (typeName == "string") {
        allowOnly("a string", {"type", "minLength", "maxLength"});
        const std::optional<long long> minLength = integer("minLength", 0, MOST_COUNT);
        const std::optional<long long> maxLength = integer("maxLength", 0, MOST_COUNT);
        // A grammar needs the upper limit, within llama.cpp's; a validated string may have none, or a larger one.
        if (!maxLength && grammarUse) {
            throw error(pointer, "a string needs 'maxLength'");
        }
        if (maxLength && grammarUse && *maxLength > GBNF_MAX_STRING_CHARS) {
            throw beyondLlamaLimit("maxLength", *maxLength, GBNF_MAX_STRING_CHARS);
        }
        if (maxLength && minLength.value_or(0) > *maxLength) {
            throw error(pointer, std::format("'minLength' {} is above 'maxLength' {}", *minLength, *maxLength));
        }
        node.kind = Node::Kind::String;
        node.minCount = static_cast<int>(minLength.value_or(0));
        node.maxCount = maxLength ? static_cast<int>(*maxLength) : UNBOUNDED;
        return node;
    }

    if (typeName == "array") {
        allowOnly("an array", {"type", "items", "minItems", "maxItems"});
        if (!schema.contains("items")) {
            throw error(pointer, "an array needs 'items'");
        }
        const std::optional<long long> minItems = integer("minItems", 0, MOST_COUNT);
        const std::optional<long long> maxItems = integer("maxItems", 0, MOST_COUNT);
        // As for a string: a grammar needs the upper limit within llama.cpp's; a validated array may have none.
        if (!maxItems && grammarUse) {
            throw error(pointer, "an array needs 'maxItems'");
        }
        if (maxItems && *maxItems < 1) {
            throw error(pointer + "/maxItems", "'maxItems' must be at least 1");
        }
        if (maxItems && grammarUse && *maxItems > GBNF_MAX_LIST_ITEMS) {
            throw beyondLlamaLimit("maxItems", *maxItems, GBNF_MAX_LIST_ITEMS);
        }
        if (maxItems && minItems.value_or(0) > *maxItems) {
            throw error(pointer, std::format("'minItems' {} is above 'maxItems' {}", *minItems, *maxItems));
        }
        node.kind = Node::Kind::Array;
        node.minCount = static_cast<int>(minItems.value_or(0));
        node.maxCount = maxItems ? static_cast<int>(*maxItems) : UNBOUNDED;
        node.children.push_back(Compile(schema.at("items"), pointer + "/items", use));
        return node;
    }

    if (typeName == "object") {
        allowOnly("an object", {"type", "properties", "required", "additionalProperties"});
        if (schema.contains("additionalProperties")) {
            const nlohmann::json& additional = schema.at("additionalProperties");
            // A grammar never allows extra keys; a validated object may be open.
            if (grammarUse && additional != false) {
                throw error(pointer + "/additionalProperties",
                            "'additionalProperties' may only be false: extra keys are never allowed");
            }
            if (!additional.is_boolean()) {
                throw error(pointer + "/additionalProperties", "'additionalProperties' must be true or false");
            }
            node.open = additional.get<bool>();
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
        // The required keys in `required` order, which the grammar writes them in (JSON objects carry no order).
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
            node.children.push_back(Compile(properties.at(key), pointer + "/properties/" + PointerToken(key), use));
        }
        node.requiredKeys = node.keys.size();
        // Then every other property: optional, which only a validated object may have. nlohmann keeps an object's
        // keys sorted, so they follow by name.
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) != node.keys.end()) {
                continue;
            }
            const std::string at = pointer + "/properties/" + PointerToken(it.key());
            if (grammarUse) {
                throw error(at, std::format("'{}' must be in 'required': optional keys are not supported", it.key()));
            }
            node.keys.push_back(it.key());
            node.children.push_back(Compile(it.value(), at, use));
        }
        return node;
    }

    throw error(pointer + "/type", std::format("the type '{}' is not supported", typeName));
}

// The grammar expression for the values `node` allows. Recursive, like Compile. Each array's item expression becomes a
// rule of `builder`, items-0, items-1 and so on in the order they are added (inner arrays first), so that ListOf repeats
// a single reference: llama.cpp limits a repetition by the symbols its repeated group creates. Only a node compiled for
// Use::Grammar comes here, so every count is bounded and every key required.
std::string Expression(const Node& node, GrammarBuilder& builder, int& itemRules)
{
    switch (node.kind) {
    case Node::Kind::Object: {
        MORPHEUS_REQUIRE(node.requiredKeys == node.keys.size() && !node.open,
                         "FromSchema: an object with optional keys or extra keys reached the grammar writer");
        // An object without keys is the empty object (Object needs at least one field).
        if (node.keys.empty()) {
            return Literal("{}");
        }
        std::vector<ObjectField> fields;
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            fields.push_back({node.keys[i], Expression(node.children[i], builder, itemRules)});
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
    case Node::Kind::Number:
    case Node::Kind::Boolean:
        throw core::ContractError("FromSchema: a number or boolean schema reached the grammar writer");
    case Node::Kind::String:
        MORPHEUS_REQUIRE(node.maxCount != UNBOUNDED, "FromSchema: an unbounded string reached the grammar writer");
        return JsonString(node.minCount, node.maxCount);
    case Node::Kind::Array: {
        MORPHEUS_REQUIRE(node.maxCount != UNBOUNDED, "FromSchema: an unbounded array reached the grammar writer");
        std::string item = Expression(node.children[0], builder, itemRules);
        const std::string name = builder.Rule(std::format("items-{}", itemRules++), std::move(item));
        return ListOf(name, node.minCount, node.maxCount);
    }
    case Node::Kind::OneOf: {
        std::vector<std::string> branches;
        for (const Node& branch : node.children) {
            branches.push_back(Expression(branch, builder, itemRules));
        }
        return Alternatives(branches);
    }
    }
    throw core::ContractError("FromSchema: unhandled schema kind");
}

// Adds to `errors` every way `value`, found at `path` inside the whole value, breaks `node`, in document order: an
// object's keys in the schema's order, then the keys it does not allow. Recursive, like Compile.
void CheckValue(const Node& node, const nlohmann::json& value, const std::string& path,
                std::vector<ValidationError>& errors)
{
    auto add = [&](const std::string& at, std::string message) { errors.push_back({at, std::move(message)}); };
    // A value as messages show it: a string in single quotes, anything else as compact JSON, both cut short.
    auto shown = [](const nlohmann::json& shownValue) {
        if (shownValue.is_string()) {
            const std::string& text = shownValue.get_ref<const std::string&>();
            const std::string cut = core::CutUtf8(text, core::SHORT_JSON_CHARS);
            return "'" + cut + (cut.size() < text.size() ? "...'" : "'");
        }
        // The replace handler keeps a string that is not UTF-8 from throwing: the value is data.
        std::string text = shownValue.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
        if (text.size() > core::SHORT_JSON_CHARS) {
            text.resize(core::SHORT_JSON_CHARS);
            text += "...";
        }
        return text;
    };
    // The allowed values as messages list them: JSON, comma-separated.
    auto listed = [](const std::vector<nlohmann::json>& listedValues) {
        std::string text;
        for (const nlohmann::json& listedValue : listedValues) {
            text += (text.empty() ? "" : ", ") + listedValue.dump();
        }
        return text;
    };
    // Keys as messages list them: 'a', 'b'.
    auto keyList = [](const std::vector<std::string>& keys, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (text.empty() ? "'" : ", '") + keys[i] + "'";
        }
        return text;
    };
    auto same = [](const nlohmann::json& a, const nlohmann::json& b) { return SameValue(a, b); };

    switch (node.kind) {
    case Node::Kind::Object: {
        if (!value.is_object()) {
            // The shape expected: empty, the required keys, or any object when none is required.
            if (node.keys.empty() && !node.open) {
                add(path, "expected an empty object");
            } else if (node.requiredKeys == 0) {
                add(path, "expected an object");
            } else {
                add(path, "expected an object with the keys " + keyList(node.keys, node.requiredKeys));
            }
            break;
        }
        // The schema's keys first, in its order: a required key must be present; every present key is checked.
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::string at = path + "/" + PointerToken(node.keys[i]);
            if (!value.contains(node.keys[i])) {
                if (i < node.requiredKeys) {
                    add(at, std::format("missing key '{}'", node.keys[i]));
                }
            } else {
                CheckValue(node.children[i], value.at(node.keys[i]), at, errors);
            }
        }
        // Then every key the schema does not allow, unless the object is open.
        if (node.open) {
            break;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                add(path + "/" + PointerToken(it.key()),
                    std::format("unexpected key '{}'; {}", it.key(),
                                node.keys.empty() ? "the object must be empty"
                                                  : "the keys are " + keyList(node.keys, node.keys.size())));
            }
        }
        break;
    }
    case Node::Kind::Enum: {
        const bool allowed = std::any_of(node.values.begin(), node.values.end(),
                                         [&](const nlohmann::json& allowedValue) { return same(allowedValue, value); });
        if (!allowed) {
            add(path, std::format("{} is not one of: {}", shown(value), listed(node.values)));
        }
        break;
    }
    case Node::Kind::Const:
        if (!same(node.values[0], value)) {
            add(path, std::format("expected {}, got {}", node.values[0].dump(), shown(value)));
        }
        break;
    case Node::Kind::Integer:
        // JsonIntInRange refuses floats and bools, and unsigned values beyond long long.
        if (!core::JsonIntInRange(value, node.minimum, node.maximum)) {
            add(path, std::format("expected an integer from {} to {}, got {}", node.minimum, node.maximum, shown(value)));
        }
        break;
    case Node::Kind::Number: {
        // The bounds the message names: both, one, or none.
        const bool hasMinimum = node.lowest != NO_MINIMUM;
        const bool hasMaximum = node.highest != NO_MAXIMUM;
        std::string range;
        if (hasMinimum && hasMaximum) {
            range = std::format(" from {} to {}", node.lowest, node.highest);
        } else if (hasMinimum) {
            range = std::format(" of at least {}", node.lowest);
        } else if (hasMaximum) {
            range = std::format(" of at most {}", node.highest);
        }
        // is_number is false for a bool. Every JSON number compares as a double.
        if (!value.is_number() || value.get<double>() < node.lowest || value.get<double>() > node.highest) {
            add(path, std::format("expected a number{}, got {}", range, shown(value)));
        }
        break;
    }
    case Node::Kind::Boolean:
        if (!value.is_boolean()) {
            add(path, std::format("expected a boolean, got {}", shown(value)));
        }
        break;
    case Node::Kind::String: {
        const bool bounded = node.maxCount != UNBOUNDED;
        if (!value.is_string()) {
            // The lengths the message names, as the schema gives them.
            std::string lengths;
            if (bounded) {
                lengths = node.minCount == 0 ? std::format(" of at most {} characters", node.maxCount)
                                             : std::format(" of {} to {} characters", node.minCount, node.maxCount);
            } else if (node.minCount > 0) {
                lengths = std::format(" of at least {} characters", node.minCount);
            }
            add(path, std::format("expected a string{}, got {}", lengths, shown(value)));
            break;
        }
        // Lengths count characters (code points), as the grammar's `char` does for compact JSON text.
        const std::size_t length = core::Utf8Length(value.get_ref<const std::string&>());
        if (bounded && length > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("longer than {} characters", node.maxCount));
        } else if (length < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("shorter than {} characters", node.minCount));
        }
        break;
    }
    case Node::Kind::Array: {
        const bool bounded = node.maxCount != UNBOUNDED;
        if (!value.is_array()) {
            if (bounded) {
                add(path, std::format("expected an array of {} to {} items, got {}", node.minCount, node.maxCount, shown(value)));
            } else if (node.minCount > 0) {
                add(path, std::format("expected an array of at least {} items, got {}", node.minCount, shown(value)));
            } else {
                add(path, std::format("expected an array, got {}", shown(value)));
            }
            break;
        }
        if (bounded && (value.size() < static_cast<std::size_t>(node.minCount) ||
                        value.size() > static_cast<std::size_t>(node.maxCount))) {
            add(path, std::format("has {} items; expected {} to {}", value.size(), node.minCount, node.maxCount));
        } else if (!bounded && value.size() < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("has {} items; expected at least {}", value.size(), node.minCount));
        }
        // Every item is checked, even past the limit.
        for (std::size_t i = 0; i < value.size(); ++i) {
            CheckValue(node.children[0], value[i], std::format("{}/{}", path, i), errors);
        }
        break;
    }
    case Node::Kind::OneOf: {
        const std::string& key = node.children[0].keys[0];
        std::vector<nlohmann::json> tags;
        for (const Node& branch : node.children) {
            tags.push_back(branch.children[0].values[0]);
        }
        if (!value.is_object()) {
            add(path, std::format("expected an object whose '{}' is one of: {}", key, listed(tags)));
            break;
        }
        const std::string at = path + "/" + PointerToken(key);
        if (!value.contains(key)) {
            add(at, std::format("missing key '{}'", key));
            break;
        }
        // The discriminator picks the branch; the value is then checked against that branch alone.
        const nlohmann::json& tag = value.at(key);
        const auto chosen = std::find_if(node.children.begin(), node.children.end(), [&](const Node& candidate) {
            return same(candidate.children[0].values[0], tag);
        });
        if (chosen == node.children.end()) {
            add(at, std::format("unknown {} {}; expected one of: {}", key, shown(tag), listed(tags)));
            break;
        }
        CheckValue(*chosen, value, path, errors);
        break;
    }
    }
}

} // namespace

std::string FromSchema(const nlohmann::json& schema)
{
    // Check the whole schema first, then write it: a bad schema never yields part of a grammar.
    const Node root = Compile(schema, "", Use::Grammar);
    GrammarBuilder builder;
    int itemRules = 0; // The item rules named so far.
    const std::string rootBody = Expression(root, builder, itemRules);
    return builder.Text(rootBody);
}

std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)
{
    // A schema outside the extended subset is the host's data error.
    const Node root = Compile(schema, "", Use::Validation);
    std::vector<ValidationError> errors;
    CheckValue(root, value, "", errors);
    return errors;
}

void CheckValidationSchema(const nlohmann::json& schema)
{
    // Compiling is the whole check: it throws for anything outside the extended subset.
    Compile(schema, "", Use::Validation);
}

} // namespace morpheus::grammar
```

- [ ] **Step 4: Run the grammar tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed`.

- [ ] **Step 5: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 434` (427 + 7), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
LC_ALL=C grep -c $'[\x80-\xFF]' src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
git add src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/ValidateExtendedTests.cpp tests/grammar/SchemaTests.cpp
git commit -m "grammar::Validate reads an extended subset for validation-only schemas; add CheckValidationSchema" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

