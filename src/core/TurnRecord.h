#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Dice.h"

namespace morpheus::core {

// One resolved check: the stat and difficulty the model asked for, the stakes it named, and what the dice did.
struct CheckOutcome {
    std::string stat = {};
    std::string difficulty = {};
    std::string stakes = {};
    CheckResult result = {};
};

// Everything one committed turn produced. Stored by store::RunStore and rendered into the transcript (L3) by
// the context module.
struct TurnRecord {
    int number = 0;
    int attempt = 0; // 0 for the first try; each regenerate adds 1 (new text sampling, same dice).
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output exactly as the model produced it.
    std::vector<CheckOutcome> checks = {};
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
    std::string narration = {};
    std::string digest = {}; // The last review's one or two sentence digest; empty when there was none.
    nlohmann::json timings = nlohmann::json::object();
    std::vector<std::string> notes = {}; // Model-written notes (assisted memory mode), committed with the turn.
};

// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
    // Hosted runs (per-speaker memory spec): whose memory this is, and from which conversation. The owned game leaves
    // them empty. A condensed summary has no conversation; a shared copy names its teller and the summary it copies.
    std::string actorId = {};
    std::string conversationId = {};
    std::string toldBy = {};
    std::int64_t sourceId = 0; // The summary a shared copy was made from; 0 otherwise.
    bool superseded = false;   // Condensed into a later summary: kept, but no longer shown.
    std::int64_t id = 0;       // The store's row id; 0 before it is stored.
};

// A model-written memory from assisted memory mode. Notes exist only for committed turns.
struct Note {
    int turn = 0;
    std::string text = {};
};

// JSON forms used for storage. Each FromJson throws DataError naming the missing or malformed member.
nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome);
CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json);
nlohmann::json AppliedChangeToJson(const AppliedChange& applied);
AppliedChange AppliedChangeFromJson(const nlohmann::json& json);
nlohmann::json RejectedChangeToJson(const RejectedChange& rejected);
RejectedChange RejectedChangeFromJson(const nlohmann::json& json);

} // namespace morpheus::core
