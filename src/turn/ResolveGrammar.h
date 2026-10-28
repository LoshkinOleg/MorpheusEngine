#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"
#include "core/World.h"

namespace morpheus::turn {

// Size limits the grammars enforce. They bound the resolve output, and so the resolution record, and keep the
// model from rambling inside JSON strings.
constexpr int MAX_CHECKS = 3;
constexpr int MAX_CHANGES = 6;        // Unconditional changes.
constexpr int MAX_BRANCH_CHANGES = 4; // Per check outcome.
constexpr int MAX_NOTES = 2;          // Assisted mode.
constexpr int MAX_LOOKUPS = 3;        // Assisted mode.
constexpr int MAX_LIST_ITEMS = 8;     // List and counter values inside a create.
constexpr int SHORT_TEXT_CHARS = 120; // intent, stakes, reasons, string values, lookup queries.
constexpr int LONG_TEXT_CHARS = 400;  // Notes.
constexpr int MAX_NEW_ID_CHARS = 40;
constexpr int MAX_ITEM_CHARS = 32;

// GBNF for the resolve call (architecture spec, "Turn flow"). The output is one compact JSON object with its keys
// in this order:
//   {"intent": str, "checks": [check], "changes": [change]}, plus "notes": [str] in assisted mode
//   check:  {"stat", "difficulty", "stakes": str, "on_success": [change], "on_failure": [change]}
//   change: {"op", "path", "value" (absent for a counter item remove), "reason": str}
// Only shapes the schema allows can be written: for every field of every entity in `state`, int set/add, enum
// set (its values), string set, list add/remove, counter item add (non-zero) and remove; and create of a
// creatable type with a new id and every field of that type in name order. Stats and difficulties are the
// world's. Applying still validates values (ranges, existing ids). `state` must be validated state.
std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode);

// GBNF for the assisted-mode lookup call: {"lookups": [{"source": "lore"|"history"|"notes", "query": str}]}, at
// most MAX_LOOKUPS entries.
std::string BuildLookupGrammar();

} // namespace morpheus::turn
