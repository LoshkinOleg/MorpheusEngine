#include "turn/ResolveGrammar.h"

#include <format>
#include <map>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "grammar/Gbnf.h"

namespace morpheus::turn {

std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode)
{
    MORPHEUS_REQUIRE(state.is_object(), "BuildResolveGrammar: state must be validated state");
    const bool assisted = mode == core::MemoryMode::Assisted;
    grammar::GrammarBuilder builder;

    std::vector<std::string> changes;          // Names of the change-N rules.
    std::map<std::string, std::string> enums;  // "type.field" -> enum-N rule name.
    int itemRuleCount = 0;                     // items-N rules written so far.

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
                const std::string name = builder.Rule(std::format("enum-{}", enums.size()), grammar::Enum(def.values));
                it = enums.emplace(typeName + "." + fieldName, name).first;
            }
            return it->second;
        }
        }
        throw core::ContractError("BuildResolveGrammar: unhandled FieldType");
    };
    // Each change shape is a rule of its own, change-N, and `change` is any one of them.
    auto addChange = [&](std::string body) {
        changes.push_back(builder.Rule(std::format("change-{}", changes.size()), std::move(body)));
    };

    // Changes to the fields of every entity that exists now. Each body is a sequence of literals around the value
    // rule; the `reason` rule closes the change object.
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const std::string typeName = entity.value().at("type").get<std::string>();
        const core::EntityTypeDef* pType = world.schema.FindEntityType(typeName);
        MORPHEUS_REQUIRE(pType != nullptr, "BuildResolveGrammar: state has an unknown entity type");
        for (const auto& [fieldName, def] : pType->fields) {
            const std::string path = entity.key() + "." + fieldName;
            const std::string pathValue = grammar::Literal(",\"path\":\"" + path + "\",\"value\":");
            switch (def.type) {
            case core::FieldType::Int:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"set", "add"}) + " " + pathValue +
                          " int reason");
                break;
            case core::FieldType::Enum:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " " +
                          valueRule(typeName, fieldName, def) + " reason");
                break;
            case core::FieldType::String:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " short-text reason");
                break;
            case core::FieldType::List:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"add", "remove"}) + " " + pathValue +
                          " short-text reason");
                break;
            case core::FieldType::Counter: {
                // The item part of the path: an item held now, written by name (so "Rusty Knife" or "water-flask"
                // can be changed), or the generic rule for a new item. Nothing held means just the generic rule.
                std::string itemRule = "item";
                const nlohmann::json& held = entity.value().at("fields").at(fieldName);
                if (!held.empty()) {
                    std::vector<std::string> names;
                    for (const auto& [itemName, count] : held.items()) {
                        // The name sits inside the JSON path string, so match its JSON-escaped form without the
                        // surrounding quotes, then escape that once more as a GBNF literal.
                        const std::string escaped = nlohmann::json(itemName).dump();
                        names.push_back(grammar::Literal(escaped.substr(1, escaped.size() - 2)));
                    }
                    names.push_back("item");
                    itemRule = builder.Rule(std::format("items-{}", itemRuleCount++), grammar::Alternatives(names));
                }
                // Add a non-zero count to the item, or remove the item (no value).
                addChange(grammar::Alternatives(
                              {grammar::Literal("{\"op\":\"add\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\",\"value\":") + " nonzero-int",
                               grammar::Literal("{\"op\":\"remove\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\"")}) +
                          " reason");
                break;
            }
            }
        }
    }

    // New entities of creatable types, every field given, in name order.
    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        if (!type.creatable) {
            continue;
        }
        std::string fields;
        for (const auto& [fieldName, def] : type.fields) {
            fields += " " + grammar::Literal((fields.empty() ? "\"" : ",\"") + fieldName + "\":") + " " +
                      valueRule(typeName, fieldName, def);
        }
        addChange(grammar::Literal("{\"op\":\"create\",\"path\":") + " new-id " +
                  grammar::Literal(",\"value\":{\"type\":\"" + typeName + "\",\"fields\":{") + fields + " " +
                  grammar::Literal("}}") + " reason");
    }

    std::vector<std::string> difficulties;
    for (const auto& [name, target] : world.difficulties) {
        difficulties.push_back(name);
    }

    // The proposal's structure: checks with their two outcome branches, the unconditional changes and, in assisted
    // mode, notes. With nothing the model could change (no entity fields, no creatable type) the change lists can
    // only be empty and there is no `change` rule to reference.
    builder.Rule("checks", grammar::ListOf("check", 0, MAX_CHECKS));
    builder.Rule("check", grammar::Object({{"stat", "stat"},
                                           {"difficulty", "difficulty"},
                                           {"stakes", "short-text"},
                                           {"on_success", "branch"},
                                           {"on_failure", "branch"}}));
    builder.Rule("branch", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_BRANCH_CHANGES));
    builder.Rule("changes", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_CHANGES));
    if (assisted) {
        builder.Rule("notes", grammar::ListOf("long-text", 0, MAX_NOTES));
        builder.Rule("long-text", grammar::JsonString(0, LONG_TEXT_CHARS));
    }
    builder.Rule("stat", grammar::Enum(world.stats));
    builder.Rule("difficulty", grammar::Enum(difficulties));
    if (!changes.empty()) {
        builder.Rule("change", grammar::Alternatives(changes));
    }

    // The value rules. Integers are exact ranges of at most 9 digits; a counter add is never zero.
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    builder.Rule("int", grammar::IntRange(-MAX_INT_MAGNITUDE, MAX_INT_MAGNITUDE));
    builder.Rule("nonzero-int", grammar::Alternatives({grammar::IntRange(-MAX_INT_MAGNITUDE, -1),
                                                       grammar::IntRange(1, MAX_INT_MAGNITUDE)}));
    builder.Rule("pos-int", grammar::IntRange(1, MAX_INT_MAGNITUDE));
    builder.Rule("item", std::format("[a-z0-9_]{{1,{}}}", MAX_ITEM_CHARS));
    builder.Rule("new-id", grammar::Literal("\"") + std::format(" [a-z0-9_:]{{1,{}}} ", MAX_NEW_ID_CHARS) +
                               grammar::Literal("\""));
    builder.Rule("text-list", grammar::ListOf("short-text", 0, MAX_LIST_ITEMS));
    // A counter value: {} or up to MAX_LIST_ITEMS new items, each with a positive count.
    builder.Rule("counter-value",
                 grammar::Alternatives({grammar::Literal("{}"),
                                        grammar::Literal("{\"") + " item " + grammar::Literal("\":") + " pos-int (" +
                                            grammar::Literal(",\"") + " item " + grammar::Literal("\":") +
                                            std::format(" pos-int){{0,{}}} ", MAX_LIST_ITEMS - 1) + grammar::Literal("}")}));
    // Every change ends with its reason, which closes the change object.
    builder.Rule("reason", grammar::Literal(",\"reason\":") + " short-text " + grammar::Literal("}"));

    std::vector<grammar::ObjectField> root = {{"intent", "short-text"}, {"checks", "checks"}, {"changes", "changes"}};
    if (assisted) {
        root.push_back({"notes", "notes"});
    }
    return builder.Text(grammar::Object(root));
}

std::string BuildLookupGrammar()
{
    grammar::GrammarBuilder builder;
    builder.Rule("lookups", grammar::ListOf("lookup", 0, MAX_LOOKUPS));
    builder.Rule("lookup", grammar::Object({{"source", grammar::Enum({"lore", "history", "notes"})},
                                            {"query", "short-text"}}));
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    return builder.Text(grammar::Object({{"lookups", "lookups"}}));
}

std::string BuildReviewGrammar(bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "BuildReviewGrammar: needs the check, the digest or both");
    grammar::GrammarBuilder builder;
    std::string review;
    if (check) {
        // Consistent with no problems, or inconsistent with 1 to REVIEW_MAX_PROBLEMS of them; the digest, when it is
        // asked for, follows the verdict.
        std::vector<grammar::ObjectField> consistent = {{"consistent", grammar::Literal("true")},
                                                        {"problems", grammar::Literal("[]")}};
        std::vector<grammar::ObjectField> inconsistent = {
            {"consistent", grammar::Literal("false")},
            {"problems", grammar::ListOf("problem-text", 1, REVIEW_MAX_PROBLEMS)}};
        if (digest) {
            consistent.push_back({"digest", "digest-text"});
            inconsistent.push_back({"digest", "digest-text"});
        }
        review = grammar::Alternatives({grammar::Object(consistent), grammar::Object(inconsistent)});
    } else {
        // The digest alone.
        review = grammar::Object({{"digest", "digest-text"}});
    }
    builder.Rule("review", review);
    if (check) {
        builder.Rule("problem-text", grammar::JsonString(1, REVIEW_PROBLEM_MAX_CHARS));
    }
    if (digest) {
        builder.Rule("digest-text", grammar::JsonString(0, DIGEST_MAX_CHARS));
    }
    // A fixed root line, whatever is asked for.
    return builder.Text("review");
}

} // namespace morpheus::turn
