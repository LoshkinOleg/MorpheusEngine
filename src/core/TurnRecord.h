#pragma once

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
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output exactly as the model produced it.
    std::vector<CheckOutcome> checks = {};
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
    std::vector<std::string> notes = {}; // Model-written notes (assisted memory mode), committed with the turn.
};

// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
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
