#include "turn/ResolveGrammar.h"

#include <format>
#include <map>
#include <string_view>
#include <vector>

#include "core/Errors.h"

namespace morpheus::turn {

namespace {

// Shared terminals. Strings are bounded JSON strings; integers have at most 9 digits, so they always fit an int.
// Placeholders: {0} short text, {1} long text, {2} item name, {3} new id length, {4} extra list items.
constexpr std::string_view TERMINALS = R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{{4}})
short-text ::= "\"" char{{0,{0}}} "\""
long-text ::= "\"" char{{0,{1}}} "\""
int ::= "0" | "-"? [1-9] [0-9]{{0,8}}
nonzero-int ::= "-"? [1-9] [0-9]{{0,8}}
pos-int ::= [1-9] [0-9]{{0,8}}
item ::= [a-z0-9_]{{1,{2}}}
new-id ::= "\"" [a-z0-9_:]{{1,{3}}} "\""
text-list ::= "[]" | "[" short-text ("," short-text){{0,{4}}} "]"
counter-value ::= "{{}}" | "{{\"" item "\":" pos-int (",\"" item "\":" pos-int){{0,{4}}} "}}"
reason ::= ",\"reason\":" short-text "}}"
)gbnf";

std::string Terminals()
{
    return std::format(TERMINALS, SHORT_TEXT_CHARS, LONG_TEXT_CHARS, MAX_ITEM_CHARS, MAX_NEW_ID_CHARS,
                       MAX_LIST_ITEMS - 1);
}

// A GBNF string literal that matches `text` exactly.
std::string Literal(std::string_view text)
{
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result + "\"";
}

// Alternatives matching each value as a JSON string.
std::string Alternatives(const std::vector<std::string>& values)
{
    MORPHEUS_REQUIRE(!values.empty(), "Alternatives: needs at least one value");
    std::string result;
    for (const std::string& value : values) {
        result += (result.empty() ? "" : " | ") + Literal(nlohmann::json(value).dump());
    }
    return result;
}

// A JSON array of 0 to maxItems `element`s.
std::string ListOf(std::string_view element, int maxItems)
{
    MORPHEUS_REQUIRE(maxItems >= 2, "ListOf: maxItems must be at least 2");
    return std::format(R"("[]" | "[" {0} ("," {0}){{0,{1}}} "]")", element, maxItems - 1);
}

} // namespace

std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode)
{
    MORPHEUS_REQUIRE(state.is_object(), "BuildResolveGrammar: state must be validated state");
    const bool assisted = mode == core::MemoryMode::Assisted;

    std::string rules;                           // change-N and enum-N definitions.
    std::vector<std::string> changes;            // Names of the change-N rules.
    std::map<std::string, std::string> enums;    // "type.field" -> enum-N rule name.
    int itemRuleCount = 0;                       // items-N rules written so far.

    // The value rule for a field; an enum gets its own rule the first time it is used.
    auto valueRule = [&](const std::string& typeName, const std::string& fieldName, const core::FieldDef& def) {
        switch (def.type) {
        case core::FieldType::Int:
            return std::string("int");
        case core::FieldType::String:
            return std::string("short-text");
        case core::FieldType::List:
            return std::string("text-list");
        case core::FieldType::Counter:
            return std::string("counter-value");
        case core::FieldType::Enum: {
            auto it = enums.find(typeName + "." + fieldName);
            if (it == enums.end()) {
                const std::string name = std::format("enum-{}", enums.size());
                rules += name + " ::= " + Alternatives(def.values) + "\n";
                it = enums.emplace(typeName + "." + fieldName, name).first;
            }
            return it->second;
        }
        }
        throw core::ContractError("BuildResolveGrammar: unhandled FieldType");
    };
    auto addChange = [&](const std::string& body) {
        changes.push_back(std::format("change-{}", changes.size()));
        rules += changes.back() + " ::= " + body + "\n";
    };

    // Changes to the fields of every entity that exists now.
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const std::string typeName = entity.value().at("type").get<std::string>();
        const core::EntityTypeDef* pType = world.schema.FindEntityType(typeName);
        MORPHEUS_REQUIRE(pType != nullptr, "BuildResolveGrammar: state has an unknown entity type");
        for (const auto& [fieldName, def] : pType->fields) {
            const std::string path = entity.key() + "." + fieldName;
            const std::string pathValue = Literal(",\"path\":\"" + path + "\",\"value\":");
            switch (def.type) {
            case core::FieldType::Int:
                addChange(Literal("{\"op\":") + " (" + Literal("\"set\"") + " | " + Literal("\"add\"") + ") " +
                          pathValue + " int reason");
                break;
            case core::FieldType::Enum:
                addChange(Literal("{\"op\":\"set\"") + " " + pathValue + " " + valueRule(typeName, fieldName, def) +
                          " reason");
                break;
            case core::FieldType::String:
                addChange(Literal("{\"op\":\"set\"") + " " + pathValue + " short-text reason");
                break;
            case core::FieldType::List:
                addChange(Literal("{\"op\":") + " (" + Literal("\"add\"") + " | " + Literal("\"remove\"") + ") " +
                          pathValue + " short-text reason");
                break;
            case core::FieldType::Counter: {
                // The item part of the path: an item held now, written by name (so "Rusty Knife" or "water-flask"
                // can be changed), or the generic rule for a new item. Nothing held means just the generic rule.
                std::string itemRule = "item";
                const nlohmann::json& held = entity.value().at("fields").at(fieldName);
                if (!held.empty()) {
                    itemRule = std::format("items-{}", itemRuleCount++);
                    std::string alternatives;
                    for (const auto& [itemName, count] : held.items()) {
                        // The name sits inside the JSON path string, so match its JSON-escaped form without the
                        // surrounding quotes, then escape that once more as a GBNF literal.
                        const std::string escaped = nlohmann::json(itemName).dump();
                        alternatives += Literal(escaped.substr(1, escaped.size() - 2)) + " | ";
                    }
                    rules += itemRule + " ::= " + alternatives + "item\n";
                }
                addChange("(" + Literal("{\"op\":\"add\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                          Literal("\",\"value\":") + " nonzero-int | " +
                          Literal("{\"op\":\"remove\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                          Literal("\"") + ") reason");
                break;
            }
            }
        }
    }

    // New entities of creatable types, every field given.
    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        if (!type.creatable) {
            continue;
        }
        std::string fields;
        for (const auto& [fieldName, def] : type.fields) {
            fields += " " + Literal((fields.empty() ? "\"" : ",\"") + fieldName + "\":") + " " +
                      valueRule(typeName, fieldName, def);
        }
        addChange(Literal("{\"op\":\"create\",\"path\":") + " new-id " +
                  Literal(",\"value\":{\"type\":\"" + typeName + "\",\"fields\":{") + fields + " " + Literal("}}") +
                  " reason");
    }

    std::vector<std::string> difficulties;
    for (const auto& [name, target] : world.difficulties) {
        difficulties.push_back(name);
    }

    std::string grammar = "root ::= " + Literal("{\"intent\":") + " short-text " + Literal(",\"checks\":") +
                          " checks " + Literal(",\"changes\":") + " changes " +
                          (assisted ? Literal(",\"notes\":") + " notes " : "") + Literal("}") + "\n";
    grammar += "checks ::= " + ListOf("check", MAX_CHECKS) + "\n";
    grammar += "check ::= " + Literal("{\"stat\":") + " stat " + Literal(",\"difficulty\":") + " difficulty " +
               Literal(",\"stakes\":") + " short-text " + Literal(",\"on_success\":") + " branch " +
               Literal(",\"on_failure\":") + " branch " + Literal("}") + "\n";
    // With nothing the model could change (no entity fields, no creatable type) the change lists can only be empty
    // and there is no `change` rule to reference.
    grammar += "branch ::= " + (changes.empty() ? Literal("[]") : ListOf("change", MAX_BRANCH_CHANGES)) + "\n";
    grammar += "changes ::= " + (changes.empty() ? Literal("[]") : ListOf("change", MAX_CHANGES)) + "\n";
    if (assisted) {
        grammar += "notes ::= " + ListOf("long-text", MAX_NOTES) + "\n";
    }
    grammar += "stat ::= " + Alternatives(world.stats) + "\n";
    grammar += "difficulty ::= " + Alternatives(difficulties) + "\n";
    if (!changes.empty()) {
        std::string changeAlternatives;
        for (const std::string& name : changes) {
            changeAlternatives += (changeAlternatives.empty() ? "" : " | ") + name;
        }
        grammar += "change ::= " + changeAlternatives + "\n";
    }
    return grammar + rules + Terminals();
}

std::string BuildLookupGrammar()
{
    std::string grammar = "root ::= " + Literal("{\"lookups\":") + " lookups " + Literal("}") + "\n";
    grammar += "lookups ::= " + ListOf("lookup", MAX_LOOKUPS) + "\n";
    grammar += "lookup ::= " + Literal("{\"source\":") + " (" + Literal("\"lore\"") + " | " +
               Literal("\"history\"") + " | " + Literal("\"notes\"") + ") " + Literal(",\"query\":") +
               " short-text " + Literal("}") + "\n";
    return grammar + Terminals();
}

} // namespace morpheus::turn
