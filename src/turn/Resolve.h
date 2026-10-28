#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"

namespace morpheus::turn {

struct ProposedCheck {
    std::string stat = {};
    std::string difficulty = {};
    std::string stakes = {};
    nlohmann::json onSuccess = nlohmann::json::array(); // Raw changes, validated when applied.
    nlohmann::json onFailure = nlohmann::json::array();
};

// The resolve call's output, read. When it cannot be read, `error` says why and everything else is empty: the turn
// goes on with nothing proposed (a model mistake, not an exception).
struct ResolveProposal {
    std::string intent = {};
    std::vector<ProposedCheck> checks = {};
    nlohmann::json changes = nlohmann::json::array(); // Raw changes, validated when applied.
    std::vector<std::string> notes = {};              // Assisted mode.
    std::string error = {};
};

// Reads the resolve output. Output cut off at the token limit, or not of the grammar's shape, gives `error`.
ResolveProposal ParseResolveOutput(std::string_view text);

// What the engine made of a proposal.
struct Resolution {
    nlohmann::json state = nullptr; // State after the applied changes.
    std::vector<core::CheckOutcome> checks = {};
    std::vector<core::AppliedChange> applied = {};
    std::vector<core::RejectedChange> rejected = {};
};

// Rolls the i-th proposed check with CheckSeed(runSeed, turn, i), so a regenerated turn keeps its rolls. Then
// applies the unconditional changes, followed by each rolled check's success or failure changes in check order.
// A check with an unknown stat or difficulty is rejected (raw: its stat, difficulty and stakes), and its changes
// are dropped. `state` must be validated state.
Resolution ApplyProposal(const core::World& world, const nlohmann::json& state, const ResolveProposal& proposal,
                         std::uint64_t runSeed, int turn);

// The user message the narrate call appends: the resolution record, then NARRATE_INSTRUCTION.
//   # Resolution
//   Intent: climb
//   Check: agility vs hard (16): rolled 11 +3 = 14, failure. Stakes: a fall
//   Applied: player.health -2
//   Rejected: add player.inventory.oil (no item 'oil' to decrease)
//
//   # Task
//   <NARRATE_INSTRUCTION>
// An unreadable proposal gets "The action could not be resolved (<error>). Nothing changed." instead. When the text
// exceeds `allowance` tokens (budget.narrate_allowance), the Rejected line goes first, then Applied entries from the
// end ("and N more"). Throws DataError if it still does not fit (narrate_allowance is too small).
std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution, int allowance,
                                 inference::LlmBackend& backend);

// One assisted-mode lookup: where to search and for what.
struct Lookup {
    store::EmbeddingKind source = store::EmbeddingKind::Lore; // lore -> Lore, history -> Turn, notes -> Note.
    std::string query = {};
};

// Reads the lookup call's output. Unreadable output gives no lookups and sets `error` (a model mistake).
std::vector<Lookup> ParseLookupOutput(std::string_view text, std::string& error);

} // namespace morpheus::turn
