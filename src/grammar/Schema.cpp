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

// Equal as the grammar sees it: a string to a string, an integer to an integer (1.0 is not 1). Integers are compared by
// their text, as the grammar's Literal writes them: nlohmann's == would equate -1 with 18446744073709551615.
bool SameValue(const nlohmann::json& a, const nlohmann::json& b)
{
    if (a.is_string() && b.is_string()) {
        return a == b;
    }
    return a.is_number_integer() && b.is_number_integer() && a.dump() == b.dump();
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
    // A count keyword above `most`, the largest a grammar llama.cpp loads can hold (see GBNF_MAX_STRING_CHARS).
    auto beyondLlamaLimit = [&](const char* keyword, long long count, long long most) {
        return error(pointer + "/" + keyword,
                     std::format("'{}' {} is above {}, the most a grammar llama.cpp loads can hold", keyword, count, most));
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

    if (typeName == "string") {
        allowOnly("a string", {"type", "minLength", "maxLength"});
        const std::optional<long long> minLength = integer("minLength", 0, MOST_COUNT);
        const std::optional<long long> maxLength = integer("maxLength", 0, MOST_COUNT);
        if (!maxLength) {
            throw error(pointer, "a string needs 'maxLength'");
        }
        if (*maxLength > GBNF_MAX_STRING_CHARS) {
            throw beyondLlamaLimit("maxLength", *maxLength, GBNF_MAX_STRING_CHARS);
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
        if (*maxItems > GBNF_MAX_LIST_ITEMS) {
            throw beyondLlamaLimit("maxItems", *maxItems, GBNF_MAX_LIST_ITEMS);
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

// The grammar expression for the values `node` allows. Recursive, like Compile. Each array's item expression becomes a
// rule of `builder`, items-0, items-1 and so on in the order they are added (inner arrays first), so that ListOf repeats
// a single reference: llama.cpp limits a repetition by the symbols its repeated group creates.
std::string Expression(const Node& node, GrammarBuilder& builder, int& itemRules)
{
    switch (node.kind) {
    case Node::Kind::Object: {
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
    case Node::Kind::String:
        return JsonString(node.minCount, node.maxCount);
    case Node::Kind::Array: {
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
    auto same = [](const nlohmann::json& a, const nlohmann::json& b) { return SameValue(a, b); };

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

std::string FromSchema(const nlohmann::json& schema)
{
    // Check the whole schema first, then write it: a bad schema never yields part of a grammar.
    const Node root = Compile(schema, "");
    GrammarBuilder builder;
    int itemRules = 0; // The item rules named so far.
    const std::string rootBody = Expression(root, builder, itemRules);
    return builder.Text(rootBody);
}

std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)
{
    // A schema outside the subset is the host's data error, the same one FromSchema throws.
    const Node root = Compile(schema, "");
    std::vector<ValidationError> errors;
    CheckValue(root, value, "", errors);
    return errors;
}

} // namespace morpheus::grammar
