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

// A call's thinking block from the turn telemetry: present only for calls that were allowed to think.
struct ThinkingStats {
    int tokens = 0; // Markers included.
    double ms = 0.0;
    bool stoppedAtLimit = false; // The block hit its thinking budget and was closed by the backend.
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
    bool stoppedAtLimit = false; // The answer hit its max_tokens.
    std::optional<ThinkingStats> thinking = {}; // nullopt when the call did not think.
};

// One retrieval hit that reached the turn's resolve prompt.
struct RetrievalRow {
    std::string source = {}; // "lore", "history" or "notes"
    std::string ref = {};
    double score = 0.0;
    std::vector<std::string> reasons = {}; // "name", "scene"; empty for similarity alone.
    std::string text = {};                 // The lore subject, the turn's input, or the note.
};

// One review call of a turn, from its telemetry.
struct ReviewRow {
    CallStats call = {};
    std::optional<bool> consistent = {}; // Set when the narration check was on and the output was readable.
    std::vector<std::string> problems = {};
    std::string digest = {};
    std::string error = {}; // Why the review output could not be read.
};

// The mark of the state node at `path`: an entity ("player"), a field ("player.location") or a counter item
// ("player.inventory.lantern"). Paths nest on '.' boundaries, as change paths do.
ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied);

// The prompt's messages, each titled with its index, role and layer: message 0 is the system message (L0-L2),
// messages before stableCount are the L3 transcript, and the rest is the tail (T1-T4).
std::vector<PromptSection> PromptSections(const inference::Prompt& prompt);

// The whole prompt as one copyable text: each message under a "=== role ===" line.
std::string RawPrompt(const inference::Prompt& prompt);

// The numbers of call `call` ("lookup", "resolve", "narrate" or "renarrate") in a turn's telemetry, with its thinking
// block when it had one; nullopt when the turn made no such call.
std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call);

// The retrieval hits in a turn's telemetry, with their text: a lore hit shows its subject (its ref), a history hit
// the input of that turn, a note hit the note ("<turn>.<index>": that turn's index-th note). Throws DataError when a
// hit names a turn or note the run does not hold.
std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history,
                                        const std::vector<core::Note>& notes);

// A turn's review calls, in the order made: one, or two when the narration was renarrated. Empty when the turn made
// none. Throws DataError when an entry is malformed.
std::vector<ReviewRow> ReviewRows(const nlohmann::json& timings);

// The first narration of a renarrated turn, the one the second replaced; nullopt when the turn was not renarrated.
// Throws DataError when the renarrate entry has no replaced narration.
std::optional<std::string> ReplacedNarration(const nlohmann::json& timings);

// The scene values automatic retrieval searched with; empty when there were none. Throws DataError when the entry is
// not a list of strings.
std::vector<std::string> SceneValues(const nlohmann::json& timings);

// A check written out: "agility vs hard: rolled 14 + 3 = 17 against 16, success". The stat part is left out when
// the dice rule adds no stat (statValue 0).
std::string CheckText(const core::CheckOutcome& check);

} // namespace morpheus::app
