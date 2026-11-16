#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace morpheus::core {

// A model that reasons before it answers writes a thinking block: the prompt ends with `open` when the call may
// think, or with `skip` (an already-empty block) when it may not. The model writes its thoughts, then `end`, and
// `afterEnd` follows before the answer. A block that reaches its token budget is closed by the backend with `close`:
// a short nudge to answer now, then `end` and `afterEnd`, so the answer starts as it does after a block that ended on
// its own. The texts are settings data: each model family spells them its own way.
struct ThinkingFormat {
    std::string open = {};     // The prompt's end when the call may think.
    std::string skip = {};     // The prompt's end when it does not (may be empty).
    std::string end = {};      // The end marker the model writes.
    std::string afterEnd = {}; // What follows the end marker before the answer.
    std::string close = {};    // What closes a block that reached its budget: a nudge, then end and afterEnd.
    bool operator==(const ThinkingFormat&) const = default;
};

// Auto: the chat template says (Qwen's block when it has the enable_thinking switch, no thinking otherwise).
// Format: the setting's own texts. None: the model has no thinking block, so the backend writes none (not a way
// to switch a thinking model's thinking off: that is Auto or Format with every thinking budget at 0, and the backend
// then writes the empty block).
enum class ThinkingMode { Auto, Format, None };

struct ThinkingSetting {
    ThinkingMode mode = ThinkingMode::Auto;
    ThinkingFormat format = {}; // Used by ThinkingMode::Format.
    bool operator==(const ThinkingSetting&) const = default;
};

// The texts Qwen's chat template writes.
inline const ThinkingFormat QWEN_THINKING = {
    "<think>\n",
    "<think>\n\n</think>\n\n",
    "</think>",
    "\n\n",
    "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n",
};

// Whether the chat template has Qwen's enable_thinking switch (it then writes the thinking block itself).
bool HasThinkingSwitch(std::string_view chatTemplate);

// The format a backend uses: Auto is QWEN_THINKING when `chatTemplate` has the enable_thinking switch and none
// otherwise; Format is its format; None is none.
std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate);

} // namespace morpheus::core
