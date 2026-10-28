#pragma once

#include <string>

#include "core/World.h"

namespace morpheus::context {

// The world's game reference for L1: how checks roll, the difficulties, the stats, and every entity type with its
// fields, so the model knows what a resolve output may name. Difficulties are in target order (ties by name),
// types and fields in name order, so the text is deterministic:
//   # Game reference
//   Checks roll 1d20+stat and succeed at or above the difficulty: easy 8, hard 16.
//   Stats (int fields of player): agility.
//   Entity types:
//   - character: agility (int 0..5), health (int 0..10), inventory (counter), location (string)
//   - npc (creatable): disposition (enum: wary, friendly)
std::string RenderWorldReference(const core::World& world);

} // namespace morpheus::context
