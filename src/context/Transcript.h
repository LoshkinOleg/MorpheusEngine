#pragma once

#include <string>
#include <vector>

#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::context {

// One turn of a transcript layer (L3), rendered: what the prompt shows, what a fold reads, and the engine's one-line
// account of it for a summary written without a model. Both sessions render their turns into entries; the prompt
// builders and the shared memory upkeep (turn::MemoryUpkeep) work on entries only.
struct TranscriptEntry {
    int turn = 0;                                      // The turn's number, run-wide.
    std::vector<inference::Message> messages = {};     // As L3 shows it: a user and an assistant message.
    std::vector<inference::Message> foldMessages = {}; // As a fold call reads it: the turn in full.
    std::string fact = {};                             // One line; SummarizeFallback writes "Turn N: <fact>".
};

// One applied change as the mechanics line and the resolution record show it, e.g. `player.health -3 (clamped
// to 0)`: set "path = value"; add "path +N" / "path -N" / "path +item"; remove "path -item" or "path removed";
// create "id created"; an engine note follows in parentheses.
std::string RenderChangePart(const core::AppliedChange& applied);

// One line recording what the engine did in a turn, for the transcript (L3):
//   [agility vs hard: 14/16 failure; player.location = "deck"; player.health -3 (clamped to 0); npc:marta created]
// Each change is a RenderChangePart. Rejected changes are left out.
// Returns "" when the turn had no checks and no applied changes.
std::string RenderMechanicsLine(const core::TurnRecord& record);

// L3: each turn becomes a user message (the input, then the mechanics line on its own line when there is one) and
// an assistant message: the narration for the newest `proseTurns` turns; for an older turn its digest when that is
// not empty, else its narration. std::numeric_limits<int>::max() shows every narration (fold calls, and the session
// with compact_transcript off). Throws ContractError for proseTurns < 1.
std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns);

// Morpheus's own turns as entries: `messages` is the turn's pair from RenderTranscript(turns, proseTurns) (an older
// turn shows its digest), `foldMessages` its pair with every narration, and `fact` its input followed by its
// mechanics line when it has one. Throws ContractError for proseTurns < 1.
std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns);

} // namespace morpheus::context
