#include "context/Transcript.h"

#include <format>

namespace morpheus::context {

std::string RenderMechanicsLine(const core::TurnRecord& record)
{
    std::vector<std::string> parts;
    for (const core::CheckOutcome& check : record.checks) {
        parts.push_back(std::format("{} vs {}: {}/{} {}", check.stat, check.difficulty, check.result.total,
                                    check.result.target, check.result.success ? "success" : "failure"));
    }
    for (const core::AppliedChange& applied : record.applied) {
        const core::Change& change = applied.change;
        std::string part;
        switch (change.op) {
        case core::ChangeOp::Set:
            part = std::format("{} = {}", change.path, change.value.dump());
            break;
        case core::ChangeOp::Add:
            // Numeric add (int field or counter item) or a list item.
            part = change.value.is_number_integer() ? std::format("{} {:+}", change.path, change.value.get<long long>())
                                                    : std::format("{} +{}", change.path, change.value.get<std::string>());
            break;
        case core::ChangeOp::Remove:
            // A list item names what was removed; a counter item is removed whole.
            part = change.value.is_string() ? std::format("{} -{}", change.path, change.value.get<std::string>())
                                            : std::format("{} removed", change.path);
            break;
        case core::ChangeOp::Create:
            part = std::format("{} created", change.path);
            break;
        }
        if (!applied.note.empty()) {
            part += std::format(" ({})", applied.note);
        }
        parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        return {};
    }
    std::string line = "[";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        line += (i == 0 ? "" : "; ") + parts[i];
    }
    return line + "]";
}

std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns)
{
    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (const core::TurnRecord& turn : turns) {
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        messages.push_back({inference::Role::Assistant, turn.narration});
    }
    return messages;
}

} // namespace morpheus::context
