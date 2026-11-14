#include "context/Transcript.h"

#include <cstddef>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "core/Errors.h"

namespace morpheus::context {

std::string RenderChangePart(const core::AppliedChange& applied)
{
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
    return part;
}

std::string RenderMechanicsLine(const core::TurnRecord& record)
{
    std::vector<std::string> parts;
    for (const core::CheckOutcome& check : record.checks) {
        parts.push_back(std::format("{} vs {}: {}/{} {}", check.stat, check.difficulty, check.result.total,
                                    check.result.target, check.result.success ? "success" : "failure"));
    }
    for (const core::AppliedChange& applied : record.applied) {
        parts.push_back(RenderChangePart(applied));
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

std::vector<inference::Message> RenderTranscript(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    MORPHEUS_REQUIRE(proseTurns >= 1, "RenderTranscript: proseTurns must be at least 1");
    // Turns from this index on are the newest proseTurns; the ones before it are older.
    const std::size_t prose = static_cast<std::size_t>(proseTurns);
    const std::size_t firstProse = turns.size() > prose ? turns.size() - prose : 0;

    std::vector<inference::Message> messages;
    messages.reserve(turns.size() * 2);
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const core::TurnRecord& turn = turns[i];
        const std::string mechanics = RenderMechanicsLine(turn);
        messages.push_back({inference::Role::User, mechanics.empty() ? turn.input : turn.input + "\n" + mechanics});
        // An older turn shows its digest. One without (played with compact_transcript off, or its review failed)
        // keeps its narration.
        const bool digested = i < firstProse && !turn.digest.empty();
        messages.push_back({inference::Role::Assistant, digested ? turn.digest : turn.narration});
    }
    return messages;
}

std::vector<TranscriptEntry> TranscriptEntries(const std::vector<core::TurnRecord>& turns, int proseTurns)
{
    // Two messages per turn in both views: as L3 shows it (older turns may show their digest) and in full, for a fold.
    const std::vector<inference::Message> shown = RenderTranscript(turns, proseTurns);
    const std::vector<inference::Message> full = RenderTranscript(turns, std::numeric_limits<int>::max());
    std::vector<TranscriptEntry> entries;
    entries.reserve(turns.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        TranscriptEntry entry;
        entry.turn = turns[i].number;
        entry.messages = {shown[2 * i], shown[2 * i + 1]};
        entry.foldMessages = {full[2 * i], full[2 * i + 1]};
        // The engine's own account of the turn: its input and, when it has one, its mechanics line.
        const std::string mechanics = RenderMechanicsLine(turns[i]);
        entry.fact = mechanics.empty() ? turns[i].input : turns[i].input + " " + mechanics;
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace morpheus::context
