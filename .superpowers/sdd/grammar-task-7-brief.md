### Task 7: `Validate`

`Validate` compiles the schema exactly as `FromSchema` does (so it throws the same `DataError`) and walks the tree with the value, adding every mistake as a `ValidationError`.

The message formats (the spec asks that they name the expected shape):
- object: `expected an object with the keys 'a', 'b'` / `expected an empty object`; `missing key 'target'` (at the key's path); `unexpected key 'intent'; the keys are 'action', 'params'` / `unexpected key 'intent'; the object must be empty` (at the extra key's path).
- enum: `'Saadia' is not one of: "0001A6A4", "0001B07C"`; const: `expected 2, got 3`.
- integer: `expected an integer from 1 to 5, got 2.5`.
- string: `expected a string of at most 48 characters, got 5` / `of 2 to 6 characters`; `longer than 48 characters`; `shorter than 2 characters`.
- array: `expected an array of 0 to 2 items, got '3'`; `has 4 items; expected 1 to 3`.
- oneOf: `expected an object whose 'action' is one of: "none", ...`; `missing key 'action'`; `unknown action 'dance'; expected one of: "none", ...` (both at the discriminator's path).
- A value is shown as a string in single quotes, anything else as compact JSON; both are cut at 80 bytes (`core::SHORT_JSON_CHARS`) with `...`.

**Files:**
- Modify: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`
- Test: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: Task 6's `Node`, `Compile`, `PointerToken`; `core::Utf8Length`, `core::CutUtf8`, `core::SHORT_JSON_CHARS` (`core/Text.h`); `core::JsonIntInRange`.
- Produces: `struct grammar::ValidationError { std::string path = {}; std::string message = {}; };` and `std::vector<grammar::ValidationError> grammar::Validate(const nlohmann::json& value, const nlohmann::json& schema);`.

- [ ] **Step 1: Make every schema-error test check `Validate` too**

In `tests/grammar/SchemaTests.cpp`, replace

```cpp
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
```

with

```cpp
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
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/grammar/SchemaTests.cpp`:

```cpp
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
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "boolean"})"_json), core::DataError);
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
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/grammar/SchemaTests.cpp`: `'Validate': is not a member of 'morpheus::grammar'` and `'ValidationError': is not a member of 'morpheus::grammar'`.

- [ ] **Step 4: Declare `Validate`**

In `src/grammar/Schema.h`, replace

```cpp
#include <string>

#include <nlohmann/json.hpp>
```

with

```cpp
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
```

and replace

```cpp
std::string FromSchema(const nlohmann::json& schema);
```

with

```cpp
std::string FromSchema(const nlohmann::json& schema);

// One mistake Validate found in a value.
struct ValidationError {
    std::string path = {};    // JSON pointer into the value, "" for the value itself.
    std::string message = {}; // What is wrong, naming the expected shape.
};

// Checks `value` against `schema`: exactly what FromSchema's grammar allows. Types (an integer is a JSON integer, not
// a float), enum and const, integer ranges, string lengths in characters (core::Utf8Length), required keys present
// and no extra keys, array sizes and each item. For a oneOf the discriminator's value picks the branch: a missing
// discriminator, or a value naming no branch, is one error at the discriminator's path; otherwise the errors are the
// chosen branch's, with full paths. Every error is reported, in document order: an object's keys in the schema's
// order (the order the grammar writes them), each with its nested errors, then the keys the schema does not allow.
// An empty result means the value is valid. Never throws on the value: a mistake in the value is data. A schema
// outside the subset throws the same DataError as FromSchema.
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);
```

- [ ] **Step 5: Implement it**

In `src/grammar/Schema.cpp`, replace

```cpp
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
```

with

```cpp
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "core/Text.h"
#include "grammar/Gbnf.h"
```

replace

```cpp
    throw core::ContractError("FromSchema: unhandled schema kind");
}

} // namespace
```

with

```cpp
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
    // Equal as the grammar sees it: a string to a string, an integer to an integer (1.0 is not 1).
    auto same = [](const nlohmann::json& a, const nlohmann::json& b) {
        return (a.is_string() && b.is_string() && a == b) || (a.is_number_integer() && b.is_number_integer() && a == b);
    };

    switch (node.kind) {
    case Node::Kind::Object: {
        std::string keyList;
        for (const std::string& key : node.keys) {
            keyList += (keyList.empty() ? "'" : ", '") + key + "'";
        }
        if (!value.is_object()) {
            add(path, node.keys.empty() ? "expected an empty object" : "expected an object with the keys " + keyList);
            break;
        }
        // The schema's keys first, in its order; each present one is checked against its schema.
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::string at = path + "/" + PointerToken(node.keys[i]);
            if (!value.contains(node.keys[i])) {
                add(at, std::format("missing key '{}'", node.keys[i]));
            } else {
                CheckValue(node.children[i], value.at(node.keys[i]), at, errors);
            }
        }
        // Then every key the schema does not allow.
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                add(path + "/" + PointerToken(it.key()),
                    std::format("unexpected key '{}'; {}", it.key(),
                                node.keys.empty() ? "the object must be empty" : "the keys are " + keyList));
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
    case Node::Kind::String: {
        if (!value.is_string()) {
            const std::string lengths = node.minCount == 0 ? std::format("at most {}", node.maxCount)
                                                           : std::format("{} to {}", node.minCount, node.maxCount);
            add(path, std::format("expected a string of {} characters, got {}", lengths, shown(value)));
            break;
        }
        // Lengths count characters (code points), as the grammar's `char` does for compact JSON text.
        const std::size_t length = core::Utf8Length(value.get_ref<const std::string&>());
        if (length > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("longer than {} characters", node.maxCount));
        } else if (length < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("shorter than {} characters", node.minCount));
        }
        break;
    }
    case Node::Kind::Array: {
        if (!value.is_array()) {
            add(path, std::format("expected an array of {} to {} items, got {}", node.minCount, node.maxCount, shown(value)));
            break;
        }
        if (value.size() < static_cast<std::size_t>(node.minCount) || value.size() > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("has {} items; expected {} to {}", value.size(), node.minCount, node.maxCount));
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
```

and append, after `FromSchema`'s definition and before the closing `} // namespace morpheus::grammar`:

```cpp
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)
{
    // A schema outside the subset is the host's data error, the same one FromSchema throws.
    const Node root = Compile(schema, "");
    std::vector<ValidationError> errors;
    CheckValue(root, value, "", errors);
    return errors;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed` (19 test cases: Task 6's 13, each schema error now also checked against `Validate`, and these 6).

- [ ] **Step 7: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 354`.

- [ ] **Step 8: Commit**

```bash
git add src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/SchemaTests.cpp
git commit -m "Add Validate: a value checked against the schema subset, mistakes as data" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

