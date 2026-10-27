#include "core/TurnRecord.h"

#include <format>
#include <limits>
#include <optional>

#include "core/Errors.h"
#include "core/JsonInt.h"

namespace morpheus::core {

nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome)
{
    return {
        {"stat", outcome.stat},
        {"difficulty", outcome.difficulty},
        {"stakes", outcome.stakes},
        {"rolls", outcome.result.rolls},
        {"stat_value", outcome.result.statValue},
        {"total", outcome.result.total},
        {"target", outcome.result.target},
        {"success", outcome.result.success},
    };
}

CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json)
{
    // The only writer is CheckOutcomeToJson, so any other shape means the stored data is damaged.
    auto malformed = [&](const char* member) {
        return DataError(std::format("check outcome: missing or malformed '{}' in {}", member, json.dump()));
    };
    if (!json.is_object()) {
        throw malformed("object");
    }
    auto requireString = [&](const char* key) {
        if (!json.contains(key) || !json.at(key).is_string()) {
            throw malformed(key);
        }
        return json.at(key).get<std::string>();
    };
    auto requireInt = [&](const char* key, long long lo, long long hi) {
        const std::optional<long long> value = json.contains(key) ? JsonIntInRange(json.at(key), lo, hi) : std::nullopt;
        if (!value) {
            throw malformed(key);
        }
        return *value;
    };

    CheckOutcome outcome;
    outcome.stat = requireString("stat");
    outcome.difficulty = requireString("difficulty");
    outcome.stakes = requireString("stakes");

    if (!json.contains("rolls") || !json.at("rolls").is_array()) {
        throw malformed("rolls");
    }
    for (const nlohmann::json& roll : json.at("rolls")) {
        const std::optional<long long> face = JsonIntInRange(roll, 1, 1000);
        if (!face) {
            throw malformed("rolls");
        }
        outcome.result.rolls.push_back(static_cast<int>(*face));
    }

    constexpr long long INT_LO = std::numeric_limits<int>::min();
    constexpr long long INT_HI = std::numeric_limits<int>::max();
    outcome.result.statValue = static_cast<int>(requireInt("stat_value", INT_LO, INT_HI));
    outcome.result.total = requireInt("total", std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());
    outcome.result.target = static_cast<int>(requireInt("target", INT_LO, INT_HI));

    if (!json.contains("success") || !json.at("success").is_boolean()) {
        throw malformed("success");
    }
    outcome.result.success = json.at("success").get<bool>();
    return outcome;
}

nlohmann::json AppliedChangeToJson(const AppliedChange& applied)
{
    return {{"change", ChangeToJson(applied.change)}, {"note", applied.note}};
}

AppliedChange AppliedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("change") || !json.contains("note") || !json.at("note").is_string()) {
        throw DataError(std::format("applied change: malformed {}", json.dump()));
    }
    // The stored change was valid when applied, so a parse failure means damaged data.
    std::string error;
    const std::optional<Change> change = ParseChange(json.at("change"), error);
    if (!change) {
        throw DataError(std::format("applied change: {}", error));
    }
    return {*change, json.at("note").get<std::string>()};
}

nlohmann::json RejectedChangeToJson(const RejectedChange& rejected)
{
    return {{"raw", rejected.raw}, {"reason", rejected.reason}};
}

RejectedChange RejectedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("raw") || !json.contains("reason") || !json.at("reason").is_string()) {
        throw DataError(std::format("rejected change: malformed {}", json.dump()));
    }
    return {json.at("raw"), json.at("reason").get<std::string>()};
}

} // namespace morpheus::core
