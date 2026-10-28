#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"

namespace morpheus::app {

// How the State window marks a node for the latest turn's applied changes.
enum class ChangeMark {
    None,
    Changed,  // A change targets this node, or a node it lies inside (a created entity).
    Contains, // A change targets something inside this node.
};

// One message of a prompt, for the Context window's formatted view.
struct PromptSection {
    std::string title = {}; // e.g. "#3 user - L3 transcript"
    std::string text = {};
};

// One model call's numbers from the turn telemetry (TurnRecord::timings).
struct CallStats {
    int promptTokens = 0;
    int reused = 0;    // Prompt tokens served from the KV cache.
    int evaluated = 0; // Prompt tokens computed.
    int generated = 0;
    double promptMs = 0.0;
    double generateMs = 0.0;
    bool stoppedAtLimit = false; // Generation hit its max_tokens.
};

// One retrieval hit that reached the turn's resolve prompt.
struct RetrievalRow {
    std::string source = {}; // "lore", "history" or "notes"
    std::string ref = {};
    double score = 0.0;
    std::string text = {}; // The lore subject, the turn's input, or the note.
};

// The mark of the state node at `path`: an entity ("player"), a field ("player.location") or a counter item
// ("player.inventory.lantern"). Paths nest on '.' boundaries, as change paths do.
ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied);

// The prompt's messages, each titled with its index, role and layer: message 0 is the system message (L0-L2),
// messages before stableCount are the L3 transcript, and the rest is the tail (T1-T4).
std::vector<PromptSection> PromptSections(const inference::Prompt& prompt);

// The whole prompt as one copyable text: each message under a "=== role ===" line.
std::string RawPrompt(const inference::Prompt& prompt);

// The numbers of call `call` ("lookup", "resolve" or "narrate") in a turn's telemetry; nullopt when the turn made
// no such call.
std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call);

// The retrieval hits in a turn's telemetry, with their text: a lore hit shows its subject (its ref), a history hit
// the input of that turn, a note hit the note ("<turn>.<index>": that turn's index-th note). Throws DataError when a
// hit names a turn or note the run does not hold.
std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history,
                                        const std::vector<core::Note>& notes);

// A check written out: "agility vs hard: rolled 14 + 3 = 17 against 16, success". The stat part is left out when
// the dice rule adds no stat (statValue 0).
std::string CheckText(const core::CheckOutcome& check);

} // namespace morpheus::app
