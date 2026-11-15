#include "turn/Session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>

#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"
#include "turn/ResolveGrammar.h"
#include "turn/Review.h"

namespace morpheus::turn {

namespace {

// Automatic retrieval searches all of memory.
constexpr std::array<store::EmbeddingKind, 3> ALL_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                             store::EmbeddingKind::Note};

// Stands in for the next input when maintenance measures the stable layers between turns.
constexpr std::string_view MAINTENANCE_INPUT = "(the player's next action)";

} // namespace

inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)
{
    inference::Sampling sampling;
    sampling.temperature = static_cast<float>(settings.temperature);
    sampling.topK = settings.topK;
    sampling.topP = static_cast<float>(settings.topP);
    sampling.seed = seed;
    return sampling;
}

std::string CleanInput(std::string_view input)
{
    // Keep every byte from space up, but DEL, plus newline and tab; UTF-8 sequences pass whole. Then trim.
    std::string kept;
    for (const char c : input) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t') {
            kept += c;
        }
    }
    return core::Trim(kept);
}

nlohmann::json CallTelemetry(const inference::Completion& completion, const inference::GenerationSpec& spec)
{
    nlohmann::json telemetry = {{"prompt_tokens", completion.promptTokens},  {"reused", completion.tokensReused},
                                {"evaluated", completion.tokensEvaluated},   {"generated", completion.tokensGenerated},
                                {"prompt_ms", completion.promptMs},          {"generate_ms", completion.generateMs},
                                {"stopped_at_limit", completion.stoppedAtLimit}};
    if (spec.thinkingMaxTokens > 0) {
        telemetry["thinking"] = {{"tokens", completion.thinkingTokens},
                                 {"ms", completion.thinkingMs},
                                 {"stopped_at_limit", completion.thinkingStoppedAtLimit},
                                 {"text", completion.thinking}};
    }
    return telemetry;
}

Session::Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
                 inference::LlmBackend& backend)
    : _world(std::move(world))
    , _settings(std::move(settings))
    , _store(std::move(store))
    , _pBackend(&backend)
    , _index(std::move(index))
{
    _mode = _settings.memoryMode.value_or(_world.defaultMemoryMode);
}

Session Session::Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                       std::uint64_t seed, inference::LlmBackend& backend)
{
    store::RunMeta meta;
    meta.worldId = world.id;
    meta.worldHash = world.contentHash;
    meta.modelId = core::PathText(settings.chatModel.stem());
    meta.rngSeed = seed;
    meta.settings = core::SettingsToJson(settings);
    store::RunStore store = store::RunStore::Create(file, meta, world.initialState);

    MemoryIndex index(settings.embeddingDocumentPrefix);
    try {
        index.SyncLore(world, store, backend);
    } catch (...) {
        // The run never started: close the database (a moved-from store holds no handle), then remove the file.
        { store::RunStore closing = std::move(store); }
        // A file that cannot be removed is deliberately secondary: the caller needs the error that stopped the start.
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        throw;
    }
    return Session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
}

Session Session::Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                      inference::LlmBackend& backend)
{
    store::RunStore store = store::RunStore::Open(file);
    // A hosted run (HostedSession's) is no world folder's run and has no state: refused before anything reads it, as
    // HostedSession::Open refuses an owned run.
    if (store.Meta().kind != store::RunKind::Owned) {
        throw core::DataError(
            std::format("run '{}' is a hosted run, not a run of world '{}'", core::PathText(file), world.id));
    }
    if (store.Meta().worldId != world.id) {
        throw core::DataError(std::format("run '{}' belongs to world '{}', not '{}'", core::PathText(file),
                                          store.Meta().worldId, world.id));
    }
    try {
        world.schema.ValidateState(store.StateAt(store.LatestTurn()));
    } catch (const core::DataError& error) {
        throw core::DataError(std::format("run '{}': its latest state no longer fits world '{}': {}",
                                          core::PathText(file), world.id, error.what()));
    }

    MemoryIndex index(settings.embeddingDocumentPrefix);
    index.SyncLore(world, store, backend);
    const bool worldChanged = store.Meta().worldHash != world.contentHash;
    Session session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    session._worldChanged = worldChanged;
    session._index.SyncHistory(session._store, session.FoldedThrough(), backend);
    return session;
}

std::optional<core::TurnRecord> Session::RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                                 std::stop_token stop)
{
    // Control characters go (newline and tab stay): they have no place in a prompt.
    const std::string cleaned = CleanInput(input);
    if (cleaned.empty()) {
        throw TurnError("the input is empty");
    }
    return PlayTurn(cleaned, 0, callbacks, stop);
}

void Session::Undo()
{
    const int latest = _store.LatestTurn();
    if (latest == 0) {
        throw TurnError("there is no turn to undo");
    }
    _store.TruncateAfter(latest - 1);
    // The undone turn's notes, and folded turns whose summary went with it, must no longer be retrieved.
    _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
}

std::optional<core::TurnRecord> Session::Regenerate(const TurnCallbacks& callbacks, std::stop_token stop)
{
    const int latest = _store.LatestTurn();
    if (latest == 0) {
        throw TurnError("there is no turn to regenerate");
    }
    const core::TurnRecord previous = *_store.Turn(latest);
    const nlohmann::json previousState = _store.StateAt(latest);
    // Truncating also deletes the summaries that cover the latest turn, so they are kept for a restore.
    const std::vector<core::Summary> previousSummaries = _store.Summaries();

    // Puts the previous turn back after a cancelled or failed attempt, with its summaries, and indexes it again.
    auto restore = [&] {
        _store.CommitTurn(previous, previousState);
        const bool summaryTruncated =
            std::any_of(previousSummaries.begin(), previousSummaries.end(),
                        [latest](const core::Summary& summary) { return summary.toTurn >= latest; });
        if (summaryTruncated) {
            // Those summaries ran through the latest turn, so anything the attempt folded or condensed covers turns
            // they already cover: put every summary back exactly as it was.
            _store.ReplaceSummaries(previousSummaries.front());
            for (std::size_t i = 1; i < previousSummaries.size(); ++i) {
                _store.AddSummary(previousSummaries[i]);
            }
        }
        _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
    };

    _store.TruncateAfter(latest - 1);
    try {
        // The previous attempt, its notes and the turns of any summary covering it are gone: they must not be
        // retrieved for the attempt that replaces it.
        _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
        std::optional<core::TurnRecord> record = PlayTurn(previous.input, previous.attempt + 1, callbacks, stop);
        if (!record) {
            restore();
        }
        return record;
    } catch (...) {
        restore();
        throw;
    }
}

bool Session::Maintain(std::stop_token stop)
{
    const int latest = _store.LatestTurn();
    const nlohmann::json state = _store.StateAt(latest);
    // The next input is unknown: a placeholder tail measures the stable layers. The turn checks the real tail.
    auto assemble = [&](const std::vector<RetrievalHit>&) {
        return Pass(state, {}, std::string(MAINTENANCE_INPUT), RESOLVE_INSTRUCTION);
    };
    if (!Upkeep(latest, 0).Settle(assemble, stop)) {
        return false;
    }
    _index.SyncHistory(_store, FoldedThrough(), *_pBackend);
    return true;
}

const store::RunMeta& Session::Meta() const
{
    return _store.Meta();
}

bool Session::WorldChanged() const
{
    return _worldChanged;
}

nlohmann::json Session::State()
{
    return _store.StateAt(_store.LatestTurn());
}

std::vector<core::TurnRecord> Session::History()
{
    const int latest = _store.LatestTurn();
    return latest == 0 ? std::vector<core::TurnRecord>{} : _store.Turns(1, latest);
}

std::vector<core::Summary> Session::Summaries()
{
    return _store.Summaries();
}

std::vector<core::Note> Session::Notes()
{
    return _store.Notes();
}

const context::Assembly& Session::LastAssembly() const
{
    return _lastAssembly;
}

std::optional<core::TurnRecord> Session::PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                                  std::stop_token stop)
{
    // A second review seeds with attempt + REVIEW_RETRY_OFFSET, which must stay within SamplingSeed's range.
    MORPHEUS_REQUIRE(attempt >= 0 && attempt < REVIEW_RETRY_OFFSET,
                     "PlayTurn: attempt must be in 0..REVIEW_RETRY_OFFSET-1");
    const int turn = _store.LatestTurn() + 1;
    const nlohmann::json state = _store.StateAt(turn - 1);
    const std::uint64_t seed = _store.Meta().rngSeed;
    nlohmann::json telemetry = nlohmann::json::object();
    // Telemetry spelling of a memory kind, as the lookup call spells sources.
    auto kindText = [](store::EmbeddingKind kind) {
        switch (kind) {
        case store::EmbeddingKind::Lore:
            return "lore";
        case store::EmbeddingKind::Turn:
            return "history";
        case store::EmbeddingKind::Note:
            return "notes";
        case store::EmbeddingKind::Summary:
            break; // A hosted run's source: the owned game never retrieves summaries.
        }
        throw core::ContractError("PlayTurn: unhandled EmbeddingKind");
    };

    // 1. Retrieve, with the input, the scene and the previous turn's digest. RetrievalQuery::Text cuts the query like
    // embedded documents, input first, so a very long input still fits the embedding context.
    RetrievalQuery query;
    query.input = input;
    query.scene = SceneOf(_world, state);
    if (turn > 1) {
        const std::optional<core::TurnRecord> previous = _store.Turn(turn - 1);
        MORPHEUS_REQUIRE(previous.has_value(), "PlayTurn: the previous turn is not in the store");
        query.digest = previous->digest;
    }
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, ALL_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // Assisted mode: the model may ask for lookups first. Their hits join the tail after the automatic ones, so
    // the fit loop sheds them first. lookupSources[i] is the i-th request's source, for the telemetry at commit.
    std::vector<store::EmbeddingKind> lookupSources;
    if (_mode == core::MemoryMode::Assisted) {
        std::optional<context::Assembly> lookupAssembly = Upkeep(turn, attempt).Fit(
            hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, LOOKUP_INSTRUCTION); },
            stop);
        if (!lookupAssembly) {
            return std::nullopt;
        }
        inference::GenerationSpec lookupSpec;
        lookupSpec.grammar = BuildLookupGrammar();
        lookupSpec.maxTokens = LOOKUP_MAX_TOKENS;
        lookupSpec.sampling = SamplingFrom(
            _settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Lookup, attempt));
        const inference::Completion looked = _pBackend->Complete(lookupAssembly->prompt, lookupSpec, {}, stop);
        if (looked.cancelled) {
            return std::nullopt;
        }

        // Lookups share retrieval_cap with the automatic hits: each gets what is left of it, so all retrieved
        // memory in the prompt stays within the cap.
        int capLeft = _settings.budget.retrievalCap;
        for (const RetrievalHit& hit : hits) {
            capLeft -= hit.tokens;
        }
        std::string lookupError;
        nlohmann::json lookupTelemetry = CallTelemetry(looked, lookupSpec);
        lookupTelemetry["requests"] = nlohmann::json::array();
        for (const Lookup& lookup : ParseLookupOutput(looked.text, lookupError)) {
            const std::array<store::EmbeddingKind, 1> source = {lookup.source};
            nlohmann::json added = nlohmann::json::array();
            // A lookup searches with its own query only (RetrievalQuery::Text cuts it like the automatic one).
            for (RetrievalHit& hit :
                 _index.Retrieve(RetrievalQuery{lookup.query}, source, capLeft, LOOKUP_MAX_HITS, *_pBackend)) {
                const bool known = std::any_of(hits.begin(), hits.end(), [&hit](const RetrievalHit& other) {
                    return other.kind == hit.kind && other.refId == hit.refId;
                });
                if (!known) {
                    added.push_back(hit.refId);
                    capLeft -= hit.tokens;
                    hits.push_back(std::move(hit));
                }
            }
            lookupSources.push_back(lookup.source);
            lookupTelemetry["requests"].push_back(
                {{"source", kindText(lookup.source)}, {"query", lookup.query}, {"added", std::move(added)}});
        }
        if (!lookupError.empty()) {
            lookupTelemetry["error"] = lookupError;
        }
        telemetry["lookup"] = std::move(lookupTelemetry);
    }

    // 2. Resolve.
    std::optional<context::Assembly> assembly = Upkeep(turn, attempt).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(state, kept, input, RESOLVE_INSTRUCTION); },
        stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = BuildResolveGrammar(_world, state, _mode);
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.thinkingMaxTokens = _settings.budget.resolveThinkingTokens;
    resolveSpec.sampling =
        SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, attempt));
    const inference::Completion resolved = _pBackend->Complete(assembly->prompt, resolveSpec, {}, stop);
    if (resolved.cancelled) {
        return std::nullopt;
    }

    // 3. Apply.
    const ResolveProposal proposal = ParseResolveOutput(resolved.text);
    Resolution resolution = ApplyProposal(_world, state, proposal, seed, turn);
    core::TurnRecord record;
    record.number = turn;
    record.attempt = attempt;
    record.input = input;
    const nlohmann::json parsed = nlohmann::json::parse(resolved.text, nullptr, false);
    record.resolve = parsed.is_discarded() ? nlohmann::json{{"unparsed", resolved.text}} : parsed;
    record.checks = resolution.checks;
    record.applied = resolution.applied;
    record.rejected = resolution.rejected;
    if (_mode == core::MemoryMode::Assisted) {
        record.notes = proposal.notes;
    }
    if (callbacks.onResolved) {
        callbacks.onResolved(record);
    }

    // 4. Narrate: the resolve prompt plus the resolve output and the narrate message, so all of the resolve prompt
    // stays cached. `problems` is empty for the first narration; a renarrate passes the review's, which the narrate
    // message shows before its instruction. Both stream through onNarrationToken.
    inference::GenerationSpec narrateSpec;
    narrateSpec.maxTokens = _settings.budget.narrateMaxTokens;
    narrateSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    auto narrate = [&](const std::vector<std::string>& problems, core::CallKind kind) {
        inference::Prompt narratePrompt = assembly->prompt;
        context::ExtendForNarrate(narratePrompt, resolved.text,
                                  RenderNarrateMessage(proposal, resolution, problems,
                                                       _settings.budget.narrateAllowance, *_pBackend));
        inference::GenerationSpec spec = narrateSpec;
        spec.sampling =
            SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, kind, attempt));
        return _pBackend->Complete(narratePrompt, spec, callbacks.onNarrationToken, stop);
    };
    const inference::Completion narrated = narrate({}, core::CallKind::Narrate);
    if (narrated.cancelled) {
        return std::nullopt;
    }
    record.narration = core::Trim(narrated.text);
    telemetry["narrate"] = CallTelemetry(narrated, narrateSpec);

    // 5. Review, when the narration check or the compact transcript is on: one standalone call reads the outcome
    // and the narration, and says whether they agree (the check), writes the turn's digest (the compact transcript),
    // or both.
    const bool check = _settings.narrationCheck;
    const bool digest = _settings.compactTranscript;
    if (check || digest) {
        inference::GenerationSpec reviewSpec;
        reviewSpec.grammar = BuildReviewGrammar(check, digest);
        reviewSpec.maxTokens = _settings.budget.reviewMaxTokens;
        reviewSpec.thinkingMaxTokens = _settings.budget.reviewThinkingTokens;
        telemetry["review"] = nlohmann::json::array();
        // Reviews `narration` with the sampling seed of `seedAttempt` and adds the call to timings.review; nullopt
        // when cancelled. Unreadable output is a model mistake: its Review holds only the error, so it finds nothing.
        auto review = [&](const std::string& narration, int seedAttempt) -> std::optional<Review> {
            inference::GenerationSpec spec = reviewSpec;
            spec.sampling = SamplingFrom(
                _settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Review, seedAttempt));
            const inference::Completion reviewed =
                _pBackend->Complete(ReviewPrompt(input, proposal, resolution, narration), spec, {}, stop);
            if (reviewed.cancelled) {
                return std::nullopt;
            }
            Review result = ParseReviewOutput(reviewed.text, check, digest);
            nlohmann::json entry = CallTelemetry(reviewed, spec);
            if (result.consistent) {
                entry["consistent"] = *result.consistent;
            }
            if (!result.problems.empty()) {
                entry["problems"] = result.problems;
            }
            if (!result.digest.empty()) {
                entry["digest"] = result.digest;
            }
            if (!result.error.empty()) {
                entry["error"] = result.error;
            }
            telemetry["review"].push_back(std::move(entry));
            return result;
        };

        std::optional<Review> verdict = review(record.narration, attempt);
        if (!verdict) {
            return std::nullopt;
        }
        if (verdict->consistent == false) {
            // 6. Renarrate, once: the narration contradicts the outcome. The player sees it start over, the new
            // narration streams, and its own review decides the digest. Whatever that review says, the second
            // narration is committed: there is no second renarrate.
            if (callbacks.onNarrationRestarted) {
                callbacks.onNarrationRestarted(verdict->problems);
            }
            const inference::Completion renarrated = narrate(verdict->problems, core::CallKind::Renarrate);
            if (renarrated.cancelled) {
                return std::nullopt;
            }
            telemetry["renarrate"] = CallTelemetry(renarrated, narrateSpec);
            telemetry["renarrate"]["replaced"] = record.narration;
            record.narration = core::Trim(renarrated.text);
            // The offset keeps this seed apart from a regenerated turn's first review (attempt + 1).
            verdict = review(record.narration, attempt + REVIEW_RETRY_OFFSET);
            if (!verdict) {
                return std::nullopt;
            }
        }
        record.digest = verdict->digest;
    }

    // 7. Commit.
    telemetry["resolve"] = CallTelemetry(resolved, resolveSpec);
    if (!proposal.error.empty()) {
        // Why nothing was proposed: the record's resolve output alone does not say so for JSON of the wrong shape.
        telemetry["resolve"]["error"] = proposal.error;
    }
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back(
            {{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}, {"reasons", hit.reasons}});
    }
    telemetry["scene"] = query.scene; // The scene values the automatic query searched with.
    if (telemetry.contains("lookup")) {
        // Which of each lookup's added hits the fit loops kept, so they reached the resolve prompt.
        nlohmann::json& requests = telemetry["lookup"]["requests"];
        for (std::size_t i = 0; i < requests.size(); ++i) {
            nlohmann::json kept = nlohmann::json::array();
            for (const nlohmann::json& ref : requests[i].at("added")) {
                const bool inPrompt = std::any_of(hits.begin(), hits.end(), [&](const RetrievalHit& hit) {
                    return hit.kind == lookupSources[i] && hit.refId == ref.get<std::string>();
                });
                if (inPrompt) {
                    kept.push_back(ref);
                }
            }
            requests[i]["kept"] = std::move(kept);
        }
    }
    record.timings = std::move(telemetry);
    _store.CommitTurn(record, resolution.state);
    return record;
}

FitPass Session::Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                      std::string_view instruction)
{
    context::ContextInputs inputs;
    inputs.engineRules = std::string(ENGINE_RULES);
    if (_mode == core::MemoryMode::Assisted) {
        inputs.engineRules += "\n\n" + std::string(ASSISTED_RULES);
    }
    inputs.summaries = _store.Summaries();
    // L3: the unfolded turns. With the compact transcript on, older turns show their digest; otherwise every turn
    // shows its narration. A fold always reads the full narration (TranscriptEntry::foldMessages).
    const int latest = _store.LatestTurn();
    const int firstUnfolded = FoldedThrough() + 1;
    if (firstUnfolded <= latest) {
        const int proseTurns = _settings.compactTranscript ? _settings.proseTurns : std::numeric_limits<int>::max();
        inputs.transcript = context::TranscriptEntries(_store.Turns(firstUnfolded, latest), proseTurns);
    }
    inputs.state = state;
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }
    inputs.playerInput = input;
    inputs.instruction = std::string(instruction);

    FitPass pass;
    pass.assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}

MemoryUpkeep Session::Upkeep(int turn, int attempt)
{
    // A fold's summary joins the store, and the turns it covers leave the transcript and become searchable history.
    return MemoryUpkeep(_store, *_pBackend, _settings, "the input and the state", turn, attempt,
                        [this](const core::Summary& summary) {
                            _store.AddSummary(summary);
                            _index.SyncHistory(_store, summary.toTurn, *_pBackend);
                        });
}

int Session::FoldedThrough()
{
    const std::vector<core::Summary> summaries = _store.Summaries();
    return summaries.empty() ? 0 : summaries.back().toTurn;
}

} // namespace morpheus::turn
