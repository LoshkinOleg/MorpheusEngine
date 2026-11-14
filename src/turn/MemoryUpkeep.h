#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// A turn that cannot run as asked: nothing to undo or regenerate, an empty input, or an input too long for the
// tail allowance. Nothing changed; the message is for the player (in a hosted run, for the host).
class TurnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr int FOLD_SUMMARY_DIVISOR = 4;     // A fold summary gets summary_cap / 4 tokens: several fit before a condense.
constexpr int CONDENSE_SUMMARY_DIVISOR = 2; // A condensed summary gets summary_cap / 2 tokens, so it fits its cap.

// One assembled prompt and the transcript entries its L3 shows, oldest first: what the upkeep measures and folds.
struct FitPass {
    context::Assembly assembly = {};
    std::vector<context::TranscriptEntry> transcript = {};
};

// The memory upkeep both sessions share (hosted-world spec, "Shared memory upkeep"): it keeps a prompt within its
// budgets by shedding retrieval hits, condensing the summaries and folding the oldest transcript entries into a new
// summary. It works on summaries and rendered entries only, so turn::Session passes its turns and
// turn::HostedSession its conversation's turns. Built for one turn's memory calls (their sampling seeds name the
// turn) and used at once: it holds the session's store, backend and settings by pointer.
class MemoryUpkeep {
public:
    // Assembles the prompt again for `hits`, from the store as it is now: the upkeep calls it after every fold,
    // condense and shed hit.
    using Assemble = std::function<FitPass(const std::vector<RetrievalHit>& hits)>;
    // Stores a fold's summary and makes the turns it covers searchable history: each session records what it folded
    // its own way.
    using StoreFold = std::function<void(const core::Summary& summary)>;

private:
    store::RunStore* _pStore = nullptr;         // Not owned; the session's.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; the session's.
    const core::Settings* _pSettings = nullptr; // Not owned; the session's.
    std::string _tailName = {};                 // What the tail holds, for the TurnError when it is too long.
    int _turn = 0;                              // The turn these memory calls are for (their sampling seeds).
    int _attempt = 0;
    StoreFold _storeFold = {};

public:
    // `tailName` completes "<tailName> use N tokens, over tail_allowance M". Throws ContractError for an empty
    // storeFold.
    MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                 std::string tailName, int turn, int attempt, StoreFold storeFold);

    // Assembles a prompt that fits, from the `hits` it may shed (from the end) and the store's summaries and
    // transcript. Every pass sheds a hit while the tail is over its allowance, condenses an over-cap summary, folds the
    // oldest half (at least one) of an over-budget or overflowing transcript, or sheds a hit as a last resort.
    // Returns the prompt, or nullopt when a memory call was cancelled (what it folded before stays). Throws TurnError
    // when the tail is over its allowance with no hit left or nothing fits at all, and DataError when a summary is
    // still over summary_cap right after a condense.
    std::optional<context::Assembly> Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                         std::stop_token stop);

    // Between turns (Session::Maintain): condenses while the summary is over its cap and folds the oldest half while
    // the transcript is over its budget, with no hits. False when cancelled. Throws DataError like Fit.
    bool Settle(const Assemble& assemble, std::stop_token stop);

    // An ended conversation (HostedSession::Maintain): folds until no entry is left, every remaining entry in one
    // call when it fits, condensing whenever the summary is over its cap. False when cancelled. Throws DataError
    // like Fit.
    bool FoldAll(const Assemble& assemble, std::stop_token stop);

private:
    // Folds the oldest `wanted` entries of `transcript` into a new summary, or as many of them as fit one call in the
    // context. When not even one fits, the engine writes that entry's summary without a model call
    // (SummarizeFallback). Every call folds at least one entry and hands the summary to StoreFold. False when
    // cancelled.
    bool FoldOldest(const inference::Message& systemMessage, const std::vector<context::TranscriptEntry>& transcript,
                    std::size_t wanted, std::stop_token stop);

    // Replaces every summary with one condensed summary. When the condense call does not fit the context, the engine
    // writes it without a model call (CondenseFallback). False when cancelled.
    bool Condense(const inference::Message& systemMessage, std::stop_token stop);
};

} // namespace morpheus::turn
