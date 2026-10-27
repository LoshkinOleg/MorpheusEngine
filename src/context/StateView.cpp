#include "context/StateView.h"

#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderStateView(const nlohmann::json& state)
{
    MORPHEUS_REQUIRE(state.is_object(), "RenderStateView: state must be an object");

    std::string text;
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const nlohmann::json& body = entity.value();
        MORPHEUS_REQUIRE(body.is_object() && body.contains("type") && body.contains("fields"),
                         std::format("RenderStateView: entity '{}' has no type or fields", entity.key()));
        text += std::format("{} ({}):", entity.key(), body.at("type").get<std::string>());

        const nlohmann::json& fields = body.at("fields");
        bool firstField = true;
        for (auto field = fields.begin(); field != fields.end(); ++field) {
            text += firstField ? " " : ", ";
            firstField = false;
            text += field.key();
            text += ' ';

            const nlohmann::json& value = field.value();
            if (value.is_array()) {
                // List: bare items.
                text += '[';
                for (std::size_t i = 0; i < value.size(); ++i) {
                    text += (i == 0 ? "" : ", ") + value[i].get<std::string>();
                }
                text += ']';
            } else if (value.is_object()) {
                // Counter: "item count" pairs.
                text += '{';
                bool firstItem = true;
                for (auto item = value.begin(); item != value.end(); ++item) {
                    text += std::format("{}{} {}", firstItem ? "" : ", ", item.key(), item.value().dump());
                    firstItem = false;
                }
                text += '}';
            } else {
                text += value.dump(); // Numbers bare, strings quoted and escaped.
            }
        }
        text += '\n';
    }
    return text;
}

} // namespace morpheus::context
