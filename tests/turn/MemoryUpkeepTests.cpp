#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "store/RunStore.h"
#include "turn/EngineText.h"
#include "turn/MemoryUpkeep.h"

using namespace morpheus;

namespace {

// Memory calls' replies: a condense call writes "Condensed.", a fold call "Folded.".
test::ScriptedBackend::Responder MemoryResponder()
{
    return [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content == turn::CONDENSE_INSTRUCTION ? std::string("Condensed.")
                                                                             : std::string("Folded.");
    };
}

core::Settings UpkeepSettings()
{
    core::Settings settings;
    settings.budget = {64, 64, 64, 1024, 1024, 1024, 512, 256}; // tail_allowance 1024, summary_cap 512.
    settings.resolveSampling = {0.3, 40, 0.9};
    return settings;
}

// Turn `number` as an entry: L3 shows "Shown N.", a fold reads "Narration N.".
context::TranscriptEntry Entry(int number)
{
    context::TranscriptEntry entry;
    entry.turn = number;
    entry.messages = {{inference::Role::User, std::format("Input {}.", number)},
                      {inference::Role::Assistant, std::format("Shown {}.", number)}};
    entry.foldMessages = {{inference::Role::User, std::format("Input {}.", number)},
                          {inference::Role::Assistant, std::format("Narration {}.", number)}};
    entry.fact = std::format("Input {}.", number);
    return entry;
}

// `count` retrieval hits, h1 to hN.
std::vector<turn::RetrievalHit> Hits(int count)
{
    std::vector<turn::RetrievalHit> hits;
    for (int i = 1; i <= count; ++i) {
        hits.push_back({store::EmbeddingKind::Lore, std::format("h{}", i), std::format("hit {}", i), 1.0f, 1, {}});
    }
    return hits;
}

// A stand-in for a session: `entries` are its turns, and the unfolded ones follow the last summary's range. Its
// prompt is flagged by rules the test sets: the tail is over its allowance while more than `maxHits` hits are left
// (-1: always), the summary over its cap while more than `maxSummaries` summaries are stored, and the transcript over
// its budget while more than `maxEntries` entries are unfolded. Its run-wide scope's fold adds the summary and records
// it.
struct Harness {
    store::RunStore store; // Set by the constructor.
    std::vector<context::TranscriptEntry> entries = {};
    int maxHits = 100;
    std::size_t maxSummaries = 100;
    std::size_t maxEntries = 100;
    std::vector<core::Summary> folded = {};

    Harness(const std::string& name, int firstTurn, int lastTurn)
        : store(store::RunStore::Create(test::FreshTempDir(name) / "run.db", store::RunMeta{},
                                        nlohmann::json::object()))
    {
        for (int number = firstTurn; number <= lastTurn; ++number) {
            entries.push_back(Entry(number));
        }
    }

    turn::MemoryUpkeep Upkeep(test::ScriptedBackend& backend, const core::Settings& settings)
    {
        turn::SummaryScope scope;
        scope.shown = [this] { return store.Summaries(); };
        scope.storeFold = [this](const core::Summary& summary) {
            store.AddSummary(summary);
            folded.push_back(summary);
        };
        scope.storeCondensed = [this](const core::Summary& condensed) { store.ReplaceSummaries(condensed); };
        scope.foldInstruction = std::string(turn::SUMMARIZE_INSTRUCTION);
        scope.condenseMessage = [](const std::vector<core::Summary>&) { return std::string(turn::CONDENSE_INSTRUCTION); };
        return turn::MemoryUpkeep(store, backend, settings, "the test tail", 9, 0, std::move(scope));
    }

    turn::MemoryUpkeep::Assemble Assemble()
    {
        return [this](const std::vector<turn::RetrievalHit>& hits) {
            const std::vector<core::Summary> summaries = store.Summaries();
            const int foldedThrough = summaries.empty() ? 0 : summaries.back().toTurn;
            turn::FitPass pass;
            for (const context::TranscriptEntry& entry : entries) {
                if (entry.turn > foldedThrough) {
                    pass.transcript.push_back(entry);
                }
            }
            pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
            pass.assembly.prompt.stableCount = 1;
            pass.assembly.tokens.tail = 7;
            pass.assembly.tailOverBudget = static_cast<int>(hits.size()) > maxHits;
            pass.assembly.summaryOverBudget = summaries.size() > maxSummaries;
            pass.assembly.transcriptOverBudget = pass.transcript.size() > maxEntries;
            return pass;
        };
    }
};

} // namespace

TEST_CASE("Fit sheds hits from the end while the tail is over its allowance, then names the caller's tail", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_tail", 1, 0);
    harness.maxHits = 1;
    std::vector<turn::RetrievalHit> hits = Hits(3);
    const std::optional<context::Assembly> fitted = harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {});
    REQUIRE(fitted);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].refId == "h1"); // The last hits go first.
    CHECK(backend.Calls().empty());

    // A tail over its allowance with no hit left is a TurnError naming what the caller's tail holds.
    harness.maxHits = -1;
    std::vector<turn::RetrievalHit> more = Hits(2);
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}), turn::TurnError);
    CHECK(more.empty());
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Fit(more, harness.Assemble(), {}),
                      "the test tail use 7 tokens, over tail_allowance 1024");
}

TEST_CASE("Fit folds the oldest half of the transcript and condenses an over-cap summary until the prompt fits",
          "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fit", 1, 8);
    harness.maxEntries = 2;
    harness.maxSummaries = 1;
    std::vector<turn::RetrievalHit> hits;
    REQUIRE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), {}));

    // 8 unfolded: fold 1-4. 4 unfolded: fold 5-6. Two summaries over a cap of one: condense. 7 and 8 fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 4);
    CHECK(harness.folded[0].text == "Folded.");
    CHECK(harness.folded[1].fromTurn == 5);
    CHECK(harness.folded[1].toTurn == 6);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 6);
    CHECK(summaries[0].text == "Condensed.");

    // The fold reads each entry's fold messages after the prompt's system message, with the fold's cap and seed.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0].prompt.messages.size() == 1 + 2 * 4 + 1);
    CHECK(calls[0].prompt.messages[0].content == "SYSTEM");
    CHECK(calls[0].prompt.messages[2].content == "Narration 1.");
    CHECK(calls[0].prompt.messages.back().content == std::string(turn::SUMMARIZE_INSTRUCTION));
    CHECK(calls[0].spec.maxTokens == 512 / turn::FOLD_SUMMARY_DIVISOR);
    CHECK(calls[0].spec.sampling.seed == core::SamplingSeed(0, 9, core::CallKind::Summarize, 0));
    CHECK(calls[2].prompt.messages.back().content == std::string(turn::CONDENSE_INSTRUCTION));
    CHECK(calls[2].spec.maxTokens == 512 / turn::CONDENSE_SUMMARY_DIVISOR);
}

TEST_CASE("Fit returns nothing when a memory call is cancelled, and stores no summary", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_cancel", 1, 4);
    harness.maxEntries = 2;
    std::stop_source stop;
    stop.request_stop();
    std::vector<turn::RetrievalHit> hits;
    CHECK_FALSE(harness.Upkeep(backend, settings).Fit(hits, harness.Assemble(), stop.get_token()));
    CHECK(harness.folded.empty());
    CHECK(harness.store.Summaries().empty());
}

TEST_CASE("Settle folds and condenses only while the transcript or the summary is over budget", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_settle", 1, 5);
    harness.maxEntries = 2;
    harness.maxHits = -1; // The tail is not Settle's to fix: between turns it is a placeholder.
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));

    // 5 unfolded: fold 1-2. 3 unfolded: fold 3. The 2 left fit.
    REQUIRE(harness.folded.size() == 2);
    CHECK(harness.folded[0].fromTurn == 1);
    CHECK(harness.folded[0].toTurn == 2);
    CHECK(harness.folded[1].fromTurn == 3);
    CHECK(harness.folded[1].toTurn == 3);

    // With nothing over budget it makes no call.
    const std::size_t calls = backend.Calls().size();
    CHECK(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}));
    CHECK(backend.Calls().size() == calls);
}

TEST_CASE("FoldAll folds every unfolded entry in one call when it fits, condensing on the way", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_fold_all", 5, 8);
    harness.store.AddSummary({1, 2, "Earlier."});
    harness.store.AddSummary({3, 4, "Later."});
    harness.maxSummaries = 2;
    CHECK(harness.Upkeep(backend, settings).FoldAll(harness.Assemble(), {}));

    // 5-8 fold in one call; three summaries over a cap of two condense into one; nothing is left unfolded.
    REQUIRE(harness.folded.size() == 1);
    CHECK(harness.folded[0].fromTurn == 5);
    CHECK(harness.folded[0].toTurn == 8);
    const std::vector<core::Summary> summaries = harness.store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 8);
    CHECK(backend.Calls().size() == 2);
}

TEST_CASE("A summary still over its cap right after a condense is a DataError", "[upkeep]")
{
    test::ScriptedBackend backend(16384, MemoryResponder());
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_over_cap", 1, 0);
    harness.store.AddSummary({1, 1, "Long."});
    harness.maxSummaries = 0; // Even one summary is over the cap.
    CHECK_THROWS_AS(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}), core::DataError);
    CHECK_THROWS_WITH(harness.Upkeep(backend, settings).Settle(harness.Assemble(), {}),
                      "summary_cap 512 is too small for even a condensed summary (0 tokens)");
    CHECK(backend.Calls().size() == 2); // One condense per Settle, then the throw instead of a second.
}

TEST_CASE("MemoryUpkeep reads and writes summaries only through its scope, with the scope's instructions",
          "[turn][upkeep]")
{
    // A scope over a vector: folds and condenses land in it, never in the store, and the calls carry its texts.
    std::vector<core::Summary> kept = {{1, 2, "Earlier."}, {3, 4, "Later."}};
    turn::SummaryScope scope;
    scope.shown = [&kept] { return kept; };
    scope.storeFold = [&kept](const core::Summary& summary) { kept.push_back(summary); };
    scope.storeCondensed = [&kept](const core::Summary& condensed) { kept = {condensed}; };
    scope.foldInstruction = "Fold for the scope.";
    scope.condenseMessage = [](const std::vector<core::Summary>& summaries) {
        return std::format("Condense these {} for the scope.", summaries.size());
    };
    test::ScriptedBackend backend(16384, [](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        return prompt.messages.back().content.starts_with("Condense these") ? std::string("Condensed.")
                                                                           : std::string("Folded.");
    });
    const core::Settings settings = UpkeepSettings();
    Harness harness("upkeep_scope", 5, 8);

    // The prompt follows the vector: entries after its last summary are unfolded, and more than two summaries are over
    // the cap.
    turn::MemoryUpkeep::Assemble assemble = [&](const std::vector<turn::RetrievalHit>&) {
        const int foldedThrough = kept.empty() ? 0 : kept.back().toTurn;
        turn::FitPass pass;
        for (const context::TranscriptEntry& entry : harness.entries) {
            if (entry.turn > foldedThrough) {
                pass.transcript.push_back(entry);
            }
        }
        pass.assembly.prompt.messages = {{inference::Role::System, "SYSTEM"}, {inference::Role::User, "TAIL"}};
        pass.assembly.prompt.stableCount = 1;
        pass.assembly.summaryOverBudget = kept.size() > 2;
        return pass;
    };
    turn::MemoryUpkeep upkeep(harness.store, backend, settings, "the test tail", 9, 0, scope);
    CHECK(upkeep.FoldAll(assemble, {}));

    // 5-8 fold in one call with the scope's instruction; three summaries over the cap condense into one with the
    // scope's message; the store's run-wide summaries are untouched.
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].fromTurn == 1);
    CHECK(kept[0].toTurn == 8);
    CHECK(kept[0].text == "Condensed.");
    REQUIRE(backend.Calls().size() == 2);
    CHECK(backend.Calls()[0].prompt.messages.back().content == "Fold for the scope.");
    CHECK(backend.Calls()[1].prompt.messages.back().content == "Condense these 3 for the scope.");
    CHECK(harness.store.Summaries().empty());
}
