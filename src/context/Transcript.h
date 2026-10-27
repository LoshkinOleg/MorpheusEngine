#pragma once

#include <string>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::context {

// One line recording what the engine did in a turn, for the transcript (L3):
//   [agility vs hard: 14/16 failure; player.location = "deck"; player.health -3 (clamped to 0); npc:marta created]
// set: "path = value"; add: "path +N" / "path -N" / "path +item"; remove: "path -item" or "path removed";
// create: "id created"; an engine note follows in parentheses. Rejected changes are left out.
// Returns "" when the turn had no checks and no applied changes.
std::string RenderMechanicsLine(const core::TurnRecord& record);

// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one)
// and an assistant message (the narration).
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns);

} // namespace morpheus::context
