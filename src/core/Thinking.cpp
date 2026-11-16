#include "core/Thinking.h"

#include "core/Errors.h"

namespace morpheus::core {

bool HasThinkingSwitch(std::string_view chatTemplate)
{
    return chatTemplate.find("enable_thinking") != std::string_view::npos;
}

std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate)
{
    switch (setting.mode) {
    case ThinkingMode::Auto:
        // The chat template's own enable_thinking switch marks a model that reasons in Qwen's block.
        return HasThinkingSwitch(chatTemplate) ? std::optional(QWEN_THINKING) : std::nullopt;
    case ThinkingMode::Format:
        return setting.format;
    case ThinkingMode::None:
        return std::nullopt;
    }
    throw ContractError("ResolveThinking: unhandled mode");
}

} // namespace morpheus::core
