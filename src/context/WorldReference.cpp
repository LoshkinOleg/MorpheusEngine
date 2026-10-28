#include "context/WorldReference.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderWorldReference(const core::World& world)
{
    std::vector<std::pair<std::string, int>> difficulties(world.difficulties.begin(), world.difficulties.end());
    std::stable_sort(difficulties.begin(), difficulties.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });

    std::string text = "# Game reference\n";
    text += std::format("Checks roll {}d{}{} and succeed at or above the difficulty:", world.dice.count,
                        world.dice.sides, world.dice.addStat ? "+stat" : "");
    for (std::size_t i = 0; i < difficulties.size(); ++i) {
        text += std::format("{} {} {}", i == 0 ? "" : ",", difficulties[i].first, difficulties[i].second);
    }
    text += ".\n";

    text += std::format("Stats (int fields of {}):", world.checkEntity);
    for (std::size_t i = 0; i < world.stats.size(); ++i) {
        text += (i == 0 ? " " : ", ") + world.stats[i];
    }
    text += ".\nEntity types:\n";

    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        text += std::format("- {}{}:", typeName, type.creatable ? " (creatable)" : "");
        bool first = true;
        for (const auto& [fieldName, def] : type.fields) {
            text += first ? " " : ", ";
            first = false;
            switch (def.type) {
            case core::FieldType::Int:
                text += std::format("{} (int {}..{})", fieldName, def.min, def.max);
                break;
            case core::FieldType::Enum: {
                std::string values;
                for (std::size_t i = 0; i < def.values.size(); ++i) {
                    values += (i == 0 ? "" : ", ") + def.values[i];
                }
                text += std::format("{} (enum: {})", fieldName, values);
                break;
            }
            case core::FieldType::String:
            case core::FieldType::List:
            case core::FieldType::Counter:
                text += std::format("{} ({})", fieldName, core::FieldTypeName(def.type));
                break;
            }
        }
        text += '\n';
    }
    return text;
}

} // namespace morpheus::context
