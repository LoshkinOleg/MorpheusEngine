#include "turn/Session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/Folding.h"
#include "turn/Resolve.h"
#include "turn/ResolveGrammar.h"

namespace morpheus::turn {

namespace {

// Automatic retrieval searches all of memory.
constexpr std::array<store::EmbeddingKind, 3> ALL_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                             store::EmbeddingKind::Note};

// Stands in for the next input when maintenance measures the stable layers between turns.
constexpr std::string_view MAINTENANCE_INPUT = "(the player's next action)";

// One model call's numbers for the turn telemetry (TurnRecord::timings).
nlohmann::json Telemetry(const inference::Completion& completion)
{
    return {{"prompt_tokens", completion.promptTokens},  {"reused", completion.tokensReused},
            {"evaluated", completion.tokensEvaluated},   {"generated", completion.tokensGenerated},
            {"prompt_ms", completion.promptMs},          {"generate_ms", completion.generateMs},
            {"stopped_at_limit", completion.stoppedAtLimit}};
}

// Thrown when a summary is still over summary_cap right after a condense: condensing again would only repeat it.
core::DataError CondensedOverCap(int summaryCap, int summaryTokens)
{
    return core::DataError(std::format("summary_cap {} is too small for even a condensed summary ({} tokens)",
                                       summaryCap, summaryTokens));
}

} // namespace

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
    // Control characters go (newline and tab stay): they have no place in a prompt, and U+001E is the backend's
    // content marker.
    std::string kept;
    for (const char c : input) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t') {
            kept += c;
        }
    }
    const std::string cleaned = core::Trim(kept);
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
    // Every pass condenses or folds. A fold shrinks the transcript, and a summary still over its cap right after a
    // condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        // The next input is unknown: a placeholder tail measures the stable layers. The turn checks the real tail.
        const context::ContextInputs inputs = Inputs(state, {}, std::string(MAINTENANCE_INPUT), RESOLVE_INSTRUCTION);
        const context::Assembly assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
        if (assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_settings.budget.summaryCap, assembly.tokens.summary);
            }
            if (!Condense(assembly.prompt.messages.front(), latest, 0, stop)) {
                return false;
            }
            condensed = true;
            continue;
        }
        if (assembly.transcriptOverBudget && !inputs.transcript.empty()) {
            if (!FoldOldest(assembly.prompt.messages.front(), inputs.transcript, latest, 0, stop)) {
                return false;
            }
            condensed = false;
            continue;
        }
        break;
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

const context::Assembly& Session::LastAssembly() const
{
    return _lastAssembly;
}

std::optional<core::TurnRecord> Session::PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                                  std::stop_token stop)
{
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
        }
        throw core::ContractError("PlayTurn: unhandled EmbeddingKind");
    };

    // 1. Retrieve. The query is cut like embedded documents, so a very long input still fits the embedding context.
    std::vector<RetrievalHit> hits = _index.Retrieve(core::CutUtf8(input, EMBED_TEXT_CHARS), ALL_SOURCES,
                                                     _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // 2. Resolve.
    std::optional<context::Assembly> assembly =
        AssembleFitting(state, hits, input, RESOLVE_INSTRUCTION, turn, attempt, stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = BuildResolveGrammar(_world, state, _mode);
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.sampling =
        inference::SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, attempt));
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

    // 4. Narrate: the resolve prompt plus the resolve output and the resolution record, so all of it stays cached.
    inference::Prompt narratePrompt = assembly->prompt;
    context::ExtendForNarrate(narratePrompt, resolved.text,
                              RenderNarrateMessage(proposal, resolution, _settings.budget.narrateAllowance, *_pBackend));
    inference::GenerationSpec narrateSpec;
    narrateSpec.maxTokens = _settings.budget.narrateMaxTokens;
    narrateSpec.sampling =
        inference::SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Narrate, attempt));
    const inference::Completion narrated =
        _pBackend->Complete(narratePrompt, narrateSpec, callbacks.onNarrationToken, stop);
    if (narrated.cancelled) {
        return std::nullopt;
    }
    record.narration = core::Trim(narrated.text);

    // 5. Commit.
    telemetry["resolve"] = Telemetry(resolved);
    telemetry["narrate"] = Telemetry(narrated);
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back({{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}});
    }
    record.timings = std::move(telemetry);
    _store.CommitTurn(record, resolution.state);
    return record;
}

context::ContextInputs Session::Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,
                                       const std::string& input, std::string_view instruction)
{
    context::ContextInputs inputs;
    inputs.engineRules = std::string(ENGINE_RULES);
    if (_mode == core::MemoryMode::Assisted) {
        inputs.engineRules += "\n\n" + std::string(ASSISTED_RULES);
    }
    inputs.summaries = _store.Summaries();
    const int latest = _store.LatestTurn();
    const int firstUnfolded = FoldedThrough() + 1;
    if (firstUnfolded <= latest) {
        inputs.transcript = _store.Turns(firstUnfolded, latest);
    }
    inputs.state = state;
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }
    inputs.playerInput = input;
    inputs.instruction = std::string(instruction);
    return inputs;
}

std::optional<context::Assembly> Session::AssembleFitting(const nlohmann::json& state, std::vector<RetrievalHit>& hits,
                                                          const std::string& input, std::string_view instruction,
                                                          int turn, int attempt, std::stop_token stop)
{
    // Every pass sheds a hit, condenses the summary, or folds turns. Hits and the transcript only shrink, and a
    // summary still over its cap right after a condense throws, so the loop ends.
    bool condensed = false; // The summary was just condensed, with nothing folded into it since.
    for (;;) {
        const context::ContextInputs inputs = Inputs(state, hits, input, instruction);
        context::Assembly assembly = context::AssembleResolvePrompt(_world, inputs, _settings.budget, *_pBackend);
        if (assembly.Fits()) {
            return assembly;
        }
        if (assembly.tailOverBudget) {
            if (hits.empty()) {
                throw TurnError(std::format("the input and the state use {} tokens, over tail_allowance {}",
                                            assembly.tokens.tail, _settings.budget.tailAllowance));
            }
            hits.pop_back();
            continue;
        }
        if (assembly.summaryOverBudget) {
            if (condensed) {
                throw CondensedOverCap(_settings.budget.summaryCap, assembly.tokens.summary);
            }
            if (!Condense(assembly.prompt.messages.front(), turn, attempt, stop)) {
                return std::nullopt;
            }
            condensed = true;
            continue;
        }
        // The transcript is over its budget, or the whole prompt overflows. contextOverflow is the authoritative fit
        // check: transcriptBudget leaves out a few template tokens that it counts.
        if (!inputs.transcript.empty()) {
            if (!FoldOldest(assembly.prompt.messages.front(), inputs.transcript, turn, attempt, stop)) {
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

bool Session::FoldOldest(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& transcript,
                         int turn, int attempt, std::stop_token stop)
{
    const std::size_t count = std::max<std::size_t>(1, transcript.size() / 2);
    const std::vector<core::TurnRecord> oldest(transcript.begin(),
                                               transcript.begin() + static_cast<std::ptrdiff_t>(count));
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _settings.budget.summaryCap / FOLD_SUMMARY_DIVISOR);
    spec.sampling = inference::SamplingFrom(
        _settings.resolveSampling, core::SamplingSeed(_store.Meta().rngSeed, turn, core::CallKind::Summarize, attempt));
    const std::optional<core::Summary> summary = SummarizeTurns(systemMessage, oldest, spec, *_pBackend, stop);
    if (!summary) {
        return false;
    }
    _store.AddSummary(*summary);
    // Folded turns leave the transcript, so they become searchable history.
    _index.SyncHistory(_store, summary->toTurn, *_pBackend);
    return true;
}

bool Session::Condense(const inference::Message& systemMessage, int turn, int attempt, std::stop_token stop)
{
    inference::GenerationSpec spec;
    spec.maxTokens = std::max(1, _settings.budget.summaryCap / CONDENSE_SUMMARY_DIVISOR);
    spec.sampling = inference::SamplingFrom(
        _settings.resolveSampling, core::SamplingSeed(_store.Meta().rngSeed, turn, core::CallKind::Summarize, attempt));
    const std::optional<core::Summary> merged = CondenseSummaries(systemMessage, _store.Summaries(), spec, *_pBackend, stop);
    if (!merged) {
        return false;
    }
    _store.ReplaceSummaries(*merged);
    return true;
}

int Session::FoldedThrough()
{
    const std::vector<core::Summary> summaries = _store.Summaries();
    return summaries.empty() ? 0 : summaries.back().toTurn;
}

} // namespace morpheus::turn
