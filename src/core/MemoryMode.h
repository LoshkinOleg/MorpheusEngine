#pragma once

#include <optional>
#include <string_view>

namespace morpheus::core {

// Who manages memory beyond the engine's deterministic retrieval (architecture spec, turn flow).
enum class MemoryMode { Engine, Assisted };

// "engine" or "assisted"; nullopt for anything else.
std::optional<MemoryMode> ParseMemoryMode(std::string_view text);

} // namespace morpheus::core
