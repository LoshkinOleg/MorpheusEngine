#include "turn/MemoryUpkeep.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "turn/Folding.h"
#include "turn/Session.h"

namespace morpheus::turn {

namespace {

// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.
core::DataError CondensedOverCap(int summaryCap, int summaryTokens)
{
    return core::DataError(std::format("summary_cap {} is too small for even a condensed summary ({} tokens)",
                                       summaryCap, summaryTokens));
}

} // namespace

MemoryUpkeep::MemoryUpkeep(store::RunStore& store, inference::LlmBackend& backend, const core::Settings& settings,
                           std::string tailName, int turn, int attempt, SummaryScope scope)
    : _pStore(&store)
    , _pBackend(&backend)
    , _pSettings(&settings)
    , _tailName(std::move(tailName))
    , _turn(turn)
    , _attempt(attempt)
    , _scope(std::move(scope))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_scope.shown) && static_cast<bool>(_scope.storeFold) &&
                         static_cast<bool>(_scope.storeCondensed) && static_cast<bool>(_scope.condenseMessage) &&
                         !_scope.foldInstruction.empty(),
                     "MemoryUpkeep: a scope needs its summaries, its stores and its instructions");
}

std::optional<context::Assembly> MemoryUpkeep::Fit(std::vector<RetrievalHit>& hits, const Assemble& assemble,
                                                   std::stop_token stop)
{
    // Every pass sheds a hit, condenses the summary, or folds turns. Hits and the transcript only shrink, and a
    // summary still over its cap right after a condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        FitPass pass = assemble(hits);
        const context::Assembly& assembly = pass.assembly;
        if (assembly.Fits()) {
            return std::move(pass.assembly);
        }
        if (assembly.tailOverBudget) {
            if (hits.empty()) {
                throw TurnError(std::format("{} use {} tokens, over tail_allowance {}", _tailName, assembly.tokens.tail,
                                            _pSettings->budget.tailAllowance));
            }
            hits.pop_back();
            continue;
        }
        if (assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, assembly.tokens.summary);
            }
            if (!Condense(assembly.prompt.messages.front(), stop)) {
                return std::nullopt;
            }
            condensed = true;
            continue;
        }
        // The transcript is over its budget, or the whole prompt overflows. contextOverflow is the authoritative fit
        // check: transcriptBudget leaves out a few template tokens that it counts.
        if (!pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return std::nullopt;
            }
            condensed = false;
            continue;
        }
        if (!hits.empty()) {
            hits.pop_back();
            continue;
        }
        throw TurnError("the prompt does not fit context_size even without transcript or retrieval");
    }
}

bool MemoryUpkeep::Settle(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds. A fold shrinks the transcript, and a summary still over its cap right after a
    // condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (pass.assembly.transcriptOverBudget && !pass.transcript.empty()) {
            const std::size_t half = std::max<std::size_t>(1, pass.transcript.size() / 2);
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, half, stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldAll(const Assemble& assemble, std::stop_token stop)
{
    // Every pass condenses or folds at least one entry, so the transcript empties and the loop ends; a summary still
    // over its cap right after a condense throws.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const FitPass pass = assemble({});
        if (pass.assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_pSettings->budget.summaryCap, pass.assembly.tokens.summary);
            }
            if (!Condense(pass.assembly.prompt.messages.front(), stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (!pass.transcript.empty()) {
            // All of it at once; FoldOldest folds the longest prefix that fits when that is too much for one call.
            if (!FoldOldest(pass.assembly.prompt.messages.front(), pass.transcript, pass.transcript.size(), stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        return true;
    }
}

bool MemoryUpkeep::FoldOldest(const inference::Message& systemMessage,
                              const std::vector<context::TranscriptEntry>& transcript, std::size_t wanted,
                              std::stop_token stop)
{
    MORPHEUS_REQUIRE(!transcript.empty() && wanted >= 1 && wanted <= transcript.size(),
                     "FoldOldest: needs 1 to transcript.size() entries to fold");
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / FOLD_SUMMARY_DIVISOR);
    spec.callTag = "summarize";
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));

    // The oldest `count` entries of the transcript.
    auto oldest = [&transcript](std::size_t count) {
        return std::vector<context::TranscriptEntry>(transcript.begin(),
                                                     transcript.begin() + static_cast<std::ptrdiff_t>(count));
    };
    // Whether a fold call over the oldest `count` entries fits the context: its prompt plus the summary's max_tokens,
    // measured exactly as the backend measures a call. The transcript can be far larger than the context (a run
    // loaded with a smaller context_size, or turns put back by undoing past a condensed summary).
    auto fits = [&](std::size_t count) {
        const int promptTokens =
            _pBackend->CountPromptTokens(SummarizePrompt(systemMessage, oldest(count), _scope.foldInstruction));
        return spec.maxTokens <= _pBackend->ContextSize() - promptTokens;
    };

    // Fold the wanted entries, or the longest prefix of them that fits. More entries never cost fewer tokens, so a
    // binary search between "fits" and "does not fit" finds that prefix with few token counts.
    std::size_t count = wanted;
    if (!fits(wanted)) {
        std::size_t fitting = 0;      // Longest prefix known to fit; 0 means none is known to.
        std::size_t tooLong = wanted; // Shortest prefix known not to fit.
        while (tooLong - fitting > 1) {
            const std::size_t middle = fitting + (tooLong - fitting) / 2;
            if (fits(middle)) {
                fitting = middle;
            } else {
                tooLong = middle;
            }
        }
        count = fitting;
    }

    std::optional<core::Summary> summary;
    if (count == 0) {
        // Not even the oldest entry fits a call: the engine writes its summary, so folding always makes progress.
        summary = SummarizeFallback(oldest(1), spec.maxTokens);
    } else {
        summary = SummarizeTurns(systemMessage, oldest(count), _scope.foldInstruction, spec, *_pBackend, stop);
        if (!summary) {
            return false;
        }
    }
    // The scope stores it and makes the folded turns searchable history.
    _scope.storeFold(*summary);
    return true;
}

bool MemoryUpkeep::Condense(const inference::Message& systemMessage, std::stop_token stop)
{
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _pSettings->budget.summaryCap / CONDENSE_SUMMARY_DIVISOR);
    spec.callTag = "condense";
    spec.sampling = SamplingFrom(_pSettings->resolveSampling, core::SamplingSeed(_pStore->Meta().rngSeed, _turn,
                                                                                core::CallKind::Summarize, _attempt));
    // The scope's shown summaries, and the condense call's message for them.
    const std::vector<core::Summary> summaries = _scope.shown();
    const std::string message = _scope.condenseMessage(summaries);

    // The condense prompt holds every shown summary (the owned game's in its system message). Summaries written under
    // a larger summary_cap can make it larger than the context: then the engine condenses them itself, so the loop
    // still makes progress.
    const int promptTokens = _pBackend->CountPromptTokens(CondensePrompt(systemMessage, message));
    std::optional<core::Summary> merged;
    if (spec.maxTokens <= _pBackend->ContextSize() - promptTokens) {
        merged = CondenseSummaries(systemMessage, summaries, message, spec, *_pBackend, stop);
        if (!merged) {
            return false;
        }
    } else {
        merged = CondenseFallback(summaries, spec.maxTokens);
    }
    // The scope replaces its shown summaries with the condensed one.
    _scope.storeCondensed(*merged);
    return true;
}

} // namespace morpheus::turn
