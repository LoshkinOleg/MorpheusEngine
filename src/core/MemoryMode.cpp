#include "core/MemoryMode.h"

namespace morpheus::core {

std::optional<MemoryMode> ParseMemoryMode(std::string_view text)
{
    if (text == "engine") {
        return MemoryMode::Engine;
    }
    if (text == "assisted") {
        return MemoryMode::Assisted;
    }
    return std::nullopt;
}

} // namespace morpheus::core
