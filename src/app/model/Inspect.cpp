#include "app/model/Inspect.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <system_error>

#include "core/Errors.h"

namespace morpheus::app {

ChangeMark MarkFor(std::string_view path, const std::vector<core::AppliedChange>& applied)
{
    // Whether `inner` is `outer` or lies inside it, nesting on '.' boundaries ("npc_10" is not inside "npc_1").
    auto within = [](std::string_view inner, std::string_view outer) {
        return inner.substr(0, outer.size()) == outer && (inner.size() == outer.size() || inner[outer.size()] == '.');
    };
    ChangeMark mark = ChangeMark::None;
    for (const core::AppliedChange& change : applied) {
        if (within(path, change.change.path)) {
            return ChangeMark::Changed;
        }
        if (within(change.change.path, path)) {
            mark = ChangeMark::Contains; // Keep looking: another change may target this node itself.
        }
    }
    return mark;
}

std::vector<PromptSection> PromptSections(const inference::Prompt& prompt)
{
    std::vector<PromptSection> sections;
    for (std::size_t i = 0; i < prompt.messages.size(); ++i) {
        const inference::Message& message = prompt.messages[i];
        const char* layer = i == 0                   ? "L0 engine rules, L1 world, L2 summary"
                            : i < prompt.stableCount ? "L3 transcript"
                                                     : "tail (T1-T4)";
        sections.push_back({std::format("#{} {} - {}", i, inference::RoleName(message.role), layer), message.content});
    }
    return sections;
}

std::string RawPrompt(const inference::Prompt& prompt)
{
    std::string text;
    for (const inference::Message& message : prompt.messages) {
        text += std::format("=== {} ===\n{}\n", inference::RoleName(message.role), message.content);
    }
    return text;
}

std::optional<CallStats> CallStatsOf(const nlohmann::json& timings, std::string_view call)
{
    const std::string key(call);
    if (!timings.is_object() || !timings.contains(key)) {
        return std::nullopt;
    }
    try {
        const nlohmann::json& numbers = timings.at(key);
        return CallStats{numbers.at("prompt_tokens").get<int>(), numbers.at("reused").get<int>(),
                         numbers.at("evaluated").get<int>(),     numbers.at("generated").get<int>(),
                         numbers.at("prompt_ms").get<double>(),  numbers.at("generate_ms").get<double>(),
                         numbers.at("stopped_at_limit").get<bool>()};
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry '{}': {}", key, error.what()));
    }
}

std::vector<RetrievalRow> RetrievalRows(const nlohmann::json& timings, const std::vector<core::TurnRecord>& history,
                                        const std::vector<core::Note>& notes)
{
    std::vector<RetrievalRow> rows;
    if (!timings.is_object() || !timings.contains("retrieval")) {
        return rows;
    }
    // A decimal number from a ref, or nullopt.
    auto number = [](std::string_view text) -> std::optional<int> {
        int value = 0;
        const auto [pNext, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || pNext != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    };
    try {
        for (const nlohmann::json& hit : timings.at("retrieval")) {
            RetrievalRow row;
            row.source = hit.at("source").get<std::string>();
            row.ref = hit.at("ref").get<std::string>();
            row.score = hit.at("score").get<double>();
            const std::string missing =
                std::format("turn telemetry names {} '{}', which the run does not hold", row.source, row.ref);
            if (row.source == "history") {
                // The ref is the turn number.
                const std::optional<int> turn = number(row.ref);
                const auto found = std::find_if(history.begin(), history.end(), [&](const core::TurnRecord& record) {
                    return turn && record.number == *turn;
                });
                if (found == history.end()) {
                    throw core::DataError(missing);
                }
                row.text = found->input;
            } else if (row.source == "notes") {
                // The ref is "<turn>.<index>": the index-th note that turn wrote.
                const std::size_t dot = row.ref.find('.');
                const std::optional<int> turn =
                    dot == std::string::npos ? std::nullopt : number(std::string_view(row.ref).substr(0, dot));
                const std::optional<int> index =
                    dot == std::string::npos ? std::nullopt : number(std::string_view(row.ref).substr(dot + 1));
                int seen = 0;
                bool found = false;
                for (const core::Note& note : notes) {
                    if (turn && index && note.turn == *turn && seen++ == *index) {
                        row.text = note.text;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw core::DataError(missing);
                }
            } else {
                row.text = row.ref; // A lore hit's ref is its subject.
            }
            rows.push_back(std::move(row));
        }
    } catch (const nlohmann::json::exception& error) {
        throw core::DataError(std::format("turn telemetry 'retrieval': {}", error.what()));
    }
    return rows;
}

std::string CheckText(const core::CheckOutcome& check)
{
    std::string rolls;
    for (const int roll : check.result.rolls) {
        rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
    }
    const int stat = check.result.statValue;
    const std::string statPart = stat == 0 ? "" : std::format(" {} {}", stat < 0 ? '-' : '+', std::abs(stat));
    return std::format("{} vs {}: rolled {}{} = {} against {}, {}", check.stat, check.difficulty, rolls, statPart,
                       check.result.total, check.result.target, check.result.success ? "success" : "failure");
}

} // namespace morpheus::app
