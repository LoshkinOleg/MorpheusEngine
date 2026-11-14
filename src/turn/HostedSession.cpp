#include "turn/HostedSession.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "grammar/Schema.h"
#include "turn/EngineText.h"
#include "turn/Session.h"

namespace morpheus::turn {

namespace {

// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
// What a hosted tail holds, for the TurnError when it does not fit tail_allowance.
constexpr std::string_view HOSTED_TAIL_NAME = "the observation, the events and the player text";
// The task line of the placeholder tail Maintain measures an ended conversation's prompt with.
constexpr std::string_view MAINTENANCE_INSTRUCTION = "(the conversation's next turn)";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
constexpr std::array<store::EmbeddingKind, 2> HOSTED_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn};
// The variables the engine fills in the hosted texts.
constexpr std::string_view SPEAKER_VARIABLE = "{speaker}";
constexpr std::string_view PLAYER_VARIABLE = "{player}";

// `text` with every {speaker} and {player} replaced by the speaker's and the player's names.
std::string FillVariables(std::string_view text, std::string_view speaker, std::string_view player)
{
    std::string filled;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string_view rest = text.substr(at);
        if (rest.starts_with(SPEAKER_VARIABLE)) {
            filled += speaker;
            at += SPEAKER_VARIABLE.size();
        } else if (rest.starts_with(PLAYER_VARIABLE)) {
            filled += player;
            at += PLAYER_VARIABLE.size();
        } else {
            filled += text[at];
            ++at;
        }
    }
    return filled;
}

} // namespace

HostedSession::HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                             inference::LlmBackend& backend)
    : _world(std::move(world))
    , _settings(std::move(settings))
    , _store(std::move(store))
    , _pBackend(&backend)
    , _index(std::move(index))
{
}

HostedSession HostedSession::Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                                  std::uint64_t seed, inference::LlmBackend& backend)
{
    // The host data is checked before the file is touched: a fault is the host's DataError.
    HostedWorld world(data);
    const nlohmann::json stored = HostDataToJson(data);
    const std::string hash = HostDataHash(data);
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }

    if (!exists) {
        // A new run: its meta and host data, then the retrievable lore indexed.
        store::RunMeta meta;
        meta.kind = store::RunKind::Hosted;
        meta.worldId = std::string(HOSTED_WORLD_ID);
        meta.worldHash = hash;
        meta.modelId = core::PathText(settings.chatModel.stem());
        meta.rngSeed = seed;
        meta.settings = core::SettingsToJson(settings);
        store::RunStore store = store::RunStore::CreateHosted(file, meta, stored);
        MemoryIndex index(settings.embeddingDocumentPrefix);
        try {
            index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
        } catch (...) {
            // The run never started: close the database (a moved-from store holds no handle), then remove the file.
            { store::RunStore closing = std::move(store); }
            // A file that cannot be removed is deliberately secondary: the caller needs the error that stopped the open.
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            throw;
        }
        return HostedSession(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    }

    // An existing run, which must be hosted. SyncLore re-embeds only the lore whose text changed (by its content
    // hash); only then does changed host data replace the stored copy, as Session::Load re-embeds before anything
    // else changes, so an open that fails leaves the stored host data and its hash as they were. Vectors stored
    // before a failure are keyed by their content hash and harmless. Then the folded turns are indexed again.
    store::RunStore store = store::RunStore::Open(file);
    if (store.Meta().kind != store::RunKind::Hosted) {
        throw core::DataError(std::format("run '{}' is an owned run of world '{}', not a hosted run",
                                          core::PathText(file), store.Meta().worldId));
    }
    MemoryIndex index(settings.embeddingDocumentPrefix);
    index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
    if (store.Meta().worldHash != hash) {
        store.ReplaceHostData(stored, hash);
    }
    HostedSession session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    session.SyncHistory();
    return session;
}

std::string HostedSession::StartConversation(const std::vector<std::string>& participantIds,
                                             const nlohmann::json& observation)
{
    const Observation observed = _world.Observe(observation);
    if (participantIds.empty()) {
        throw core::DataError("participants: a conversation needs at least one participant");
    }
    // Each participant is an actor of the observation, once; its name and bio come from there.
    std::vector<core::Participant> participants;
    for (std::size_t i = 0; i < participantIds.size(); ++i) {
        const core::Actor* pActor = observed.Find(participantIds[i]);
        if (pActor == nullptr) {
            throw core::DataError(
                std::format("participants/{}: '{}' is not an actor of the observation", i, participantIds[i]));
        }
        const bool repeated = std::any_of(participants.begin(), participants.end(),
                                          [pActor](const core::Participant& participant) { return participant.id == pActor->id; });
        if (repeated) {
            throw core::DataError(std::format("participants/{}: '{}' is listed twice", i, participantIds[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    return _store.CreateConversation(participants);
}

void HostedSession::RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    HostedWorld::CheckEvents(events);
    // They follow the conversation's latest turn (0 before its first), so they come before its next.
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    _store.AddConversationEvents(conversation.id, played.empty() ? 0 : played.back().number, events);
}

void HostedSession::SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                                    const std::vector<std::string>& left, const nlohmann::json& observation)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    const Observation observed = _world.Observe(observation);
    std::vector<core::Participant> participants = conversation.participants;
    // Where participant `id` is in the join order.
    auto position = [&participants](const std::string& id) {
        return std::find_if(participants.begin(), participants.end(),
                            [&id](const core::Participant& participant) { return participant.id == id; });
    };

    // Those who left go first, so a leave and a rejoin in one call moves the actor to the end.
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto it = position(left[i]);
        if (it == participants.end()) {
            throw core::DataError(std::format("left/{}: '{}' is not a participant of conversation '{}'", i, left[i],
                                              conversation.id));
        }
        participants.erase(it);
    }
    // Those who joined go to the end of the join order, with their names and bios from the observation.
    for (std::size_t i = 0; i < joined.size(); ++i) {
        if (position(joined[i]) != participants.end()) {
            throw core::DataError(std::format("joined/{}: '{}' is already a participant of conversation '{}'", i,
                                              joined[i], conversation.id));
        }
        const core::Actor* pActor = observed.Find(joined[i]);
        if (pActor == nullptr) {
            throw core::DataError(std::format("joined/{}: '{}' is not an actor of the observation", i, joined[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    if (participants.empty()) {
        throw core::DataError(std::format("left: conversation '{}' would have no participant left", conversation.id));
    }
    _store.SetParticipants(conversation.id, participants);
}

std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request,
                                                       const HostedCallbacks& callbacks, std::stop_token stop)
{
    // 0. Check the request: nothing is stored and no model call is made before it passes. The conversation exists and
    // is open, the observation passes Observe, the speaker takes part, is present and is not the player, every
    // allowed action is in the catalog with one left after the actor scopes, and every event has its text.
    const core::Conversation conversation = OpenConversation(request.conversationId);
    const Observation observation = _world.Observe(request.observation);
    const bool participates = std::any_of(conversation.participants.begin(), conversation.participants.end(),
                                          [&](const core::Participant& participant) { return participant.id == request.speakerId; });
    if (!participates) {
        throw core::DataError(std::format("speaker: '{}' is not a participant of conversation '{}'", request.speakerId,
                                          conversation.id));
    }
    const core::Actor* pSpeaker = observation.Find(request.speakerId);
    if (pSpeaker == nullptr) {
        throw core::DataError(std::format("speaker: '{}' is not an actor of the observation", request.speakerId));
    }
    if (pSpeaker->isPlayer) {
        throw core::DataError(
            std::format("speaker: '{}' is the player; the host speaks for the player", request.speakerId));
    }
    const std::vector<std::string> allowed = request.allowedActions.value_or(_world.ActionIds());
    const nlohmann::json proposalSchema =
        _world.ProposalSchema(observation, conversation.participants, allowed, request.speakerId);
    // The resolve call's grammar is that schema's (HostedWorld::ProposalGrammar), built once here with the schema.
    const std::string proposalGrammar = grammar::FromSchema(proposalSchema);
    HostedWorld::CheckEvents(request.events);

    // Player text is cleaned as Session cleans input: control characters other than newline and tab go (they have no
    // place in a prompt), and it is trimmed. Nothing left means no player text.
    std::optional<std::string> playerText;
    if (request.playerText) {
        std::string cleaned = CleanInput(*request.playerText);
        if (!cleaned.empty()) {
            playerText = std::move(cleaned);
        }
    }

    // The instructions name the speaker and the player. The speak instruction must fit narrate_allowance, which the
    // prompt reserves for it; the names come from the host.
    const core::Actor& player = observation.Player();
    const std::string resolveInstruction = FillVariables(HOSTED_RESOLVE_INSTRUCTION, pSpeaker->name, player.name);
    const std::string speakMessage = FillVariables(HOSTED_SPEAK_INSTRUCTION, pSpeaker->name, player.name);
    const int speakTokens = _pBackend->CountTokens(speakMessage);
    if (speakTokens > _settings.budget.narrateAllowance) {
        throw core::DataError(std::format("speaker: the speak instruction for '{}' uses {} tokens, over "
                                          "narrate_allowance {}",
                                          pSpeaker->name, speakTokens, _settings.budget.narrateAllowance));
    }

    const int turn = _store.LatestHostedTurn() + 1; // Run-wide, shared with the summaries' ranges.
    const std::uint64_t seed = _store.Meta().rngSeed;
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    const int lastTurn = played.empty() ? 0 : played.back().number;

    // The new events: those recorded since the conversation's last turn, then the request's, in order.
    std::vector<std::string> newEvents;
    for (const core::ConversationEvent& recorded : _store.ConversationEvents(conversation.id)) {
        if (recorded.afterTurn == lastTurn) {
            newEvents.push_back(recorded.event.at("text").get<std::string>());
        }
    }
    for (const nlohmann::json& event : request.events) {
        newEvents.push_back(event.at("text").get<std::string>());
    }

    // 1. Retrieve from lore and the run's folded turns (this and earlier conversations), with the player text, the
    // speaker's name and the previous line.
    RetrievalQuery query;
    query.input = playerText.value_or("");
    query.scene = {pSpeaker->name};
    query.digest = played.empty() ? std::string() : played.back().line;
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, HOSTED_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);

    // 2. Resolve: a prompt that fits (the fit loop sheds hits, condenses and folds), under the proposal grammar.
    context::HostedInputs tail;
    tail.observation = observation.json;
    tail.events = newEvents;
    tail.playerLine = playerText ? player.name + ": " + *playerText : std::string();
    tail.instruction = resolveInstruction;
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(conversation.id, kept, tail); }, stop);
    if (!assembly) {
        return std::nullopt;
    }
    _lastAssembly = *assembly;
    inference::GenerationSpec resolveSpec;
    resolveSpec.grammar = proposalGrammar;
    resolveSpec.maxTokens = _settings.budget.resolveMaxTokens;
    resolveSpec.thinkingMaxTokens = _settings.budget.resolveThinkingTokens;
    resolveSpec.sampling =
        SamplingFrom(_settings.resolveSampling, core::SamplingSeed(seed, turn, core::CallKind::Resolve, 0));
    const inference::Completion resolved = _pBackend->Complete(assembly->prompt, resolveSpec, {}, stop);
    if (resolved.cancelled) {
        return std::nullopt;
    }
    // A refused action is the model's mistake: its reasons are kept, and the speaker takes no action.
    const core::Adjudication adjudication = _world.Adjudicate(proposalSchema, resolved.text);
    if (callbacks.onAction) {
        callbacks.onAction(adjudication);
    }

    // 3. Speak: the resolve prompt, the resolve output as the model wrote it, and the speak instruction, so nearly all
    // of the prompt stays cached. No grammar; the line streams.
    inference::Prompt speakPrompt = assembly->prompt;
    context::ExtendForNarrate(speakPrompt, resolved.text, speakMessage);
    inference::GenerationSpec speakSpec;
    speakSpec.maxTokens = _settings.budget.narrateMaxTokens;
    speakSpec.thinkingMaxTokens = _settings.budget.narrateThinkingTokens;
    speakSpec.sampling =
        SamplingFrom(_settings.narrateSampling, core::SamplingSeed(seed, turn, core::CallKind::Narrate, 0));
    const inference::Completion spoken = _pBackend->Complete(speakPrompt, speakSpec, callbacks.onToken, stop);
    if (spoken.cancelled) {
        return std::nullopt;
    }

    // 4. Commit the record: the listeners are the other participants present.
    core::HostedTurn record;
    record.number = turn;
    record.conversationId = conversation.id;
    record.speakerId = request.speakerId;
    for (const core::Participant& participant : conversation.participants) {
        if (participant.id != request.speakerId && observation.Find(participant.id) != nullptr) {
            record.listenerIds.push_back(participant.id);
        }
    }
    record.playerText = playerText;
    record.events = request.events;
    record.observation = observation.json;
    record.resolveOutput = resolved.text;
    record.adjudication = adjudication;
    record.line = core::Trim(spoken.text);
    nlohmann::json timings = nlohmann::json::object();
    timings["resolve"] = CallTelemetry(resolved, resolveSpec);
    if (!adjudication.requested) {
        timings["resolve"]["refused"] = adjudication.reasons; // The log of why the speaker took no action.
    }
    timings["speak"] = CallTelemetry(spoken, speakSpec);
    timings["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        timings["retrieval"].push_back({{"source", hit.kind == store::EmbeddingKind::Lore ? "lore" : "history"},
                                        {"ref", hit.refId},
                                        {"score", hit.score},
                                        {"reasons", hit.reasons}});
    }
    record.timings = std::move(timings);
    _store.CommitHostedTurn(record);
    return record;
}

void HostedSession::EndConversation(std::string_view conversationId)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    _store.EndConversation(conversation.id);
}

bool HostedSession::Maintain(std::stop_token stop)
{
    // Every ended conversation with turns still in L3 is folded whole. Its seeds name the run's latest turn.
    const int latest = _store.LatestHostedTurn();
    for (const core::Conversation& conversation : _store.Conversations()) {
        if (!conversation.ended) {
            continue;
        }
        const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
        if (played.empty() || played.back().number <= conversation.foldedThrough) {
            continue;
        }
        // The prompt the conversation's next turn would start with: its last observation and a placeholder task stand
        // in for the tail, which no fold reads.
        context::HostedInputs tail;
        tail.observation = played.back().observation;
        tail.instruction = std::string(MAINTENANCE_INSTRUCTION);
        auto assemble = [&](const std::vector<RetrievalHit>&) { return Pass(conversation.id, {}, tail); };
        // Each fold's summary is stored and its turns indexed as it lands (Upkeep's StoreFold).
        if (!Upkeep(conversation.id, latest).FoldAll(assemble, stop)) {
            return false;
        }
    }
    return true;
}

const store::RunMeta& HostedSession::Meta() const
{
    return _store.Meta();
}

std::optional<core::Conversation> HostedSession::Conversation(std::string_view id)
{
    return _store.Conversation(id);
}

std::vector<core::HostedTurn> HostedSession::Turns(std::string_view conversationId)
{
    return _store.HostedTurns(conversationId);
}

std::vector<core::ConversationEvent> HostedSession::Events(std::string_view conversationId)
{
    return _store.ConversationEvents(conversationId);
}

std::vector<core::Summary> HostedSession::Summaries()
{
    return _store.Summaries();
}

const context::Assembly& HostedSession::LastAssembly() const
{
    return _lastAssembly;
}

core::Conversation HostedSession::OpenConversation(std::string_view conversationId)
{
    std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    if (!conversation) {
        throw core::DataError(std::format("conversation_id: there is no conversation '{}'", conversationId));
    }
    if (conversation->ended) {
        throw core::DataError(std::format("conversation_id: conversation '{}' has ended", conversationId));
    }
    return std::move(*conversation);
}

std::vector<context::TranscriptEntry> HostedSession::Entries(const core::Conversation& conversation, bool folded)
{
    const std::vector<core::ConversationEvent> recorded = _store.ConversationEvents(conversation.id);
    std::vector<context::TranscriptEntry> entries;
    int previous = 0; // The conversation's turn before the one rendered; 0 before its first.
    for (const core::HostedTurn& turn : _store.HostedTurns(conversation.id)) {
        if ((turn.number <= conversation.foldedThrough) == folded) {
            // Names from the turn's own observation, as it was when the turn was played. RunStore::HostedTurns checked
            // its actors core and that the speaker is among them (a damaged turn is its StoreError), so this read
            // cannot fail and the speaker and the player are found.
            const std::vector<core::Actor> actors = HostedWorld::ActorsOf(turn.observation);
            const auto speaker = std::find_if(actors.begin(), actors.end(),
                                              [&turn](const core::Actor& actor) { return actor.id == turn.speakerId; });
            const auto player = std::find_if(actors.begin(), actors.end(), [](const core::Actor& actor) { return actor.isPlayer; });
            MORPHEUS_REQUIRE(speaker != actors.end() && player != actors.end(),
                             "HostedSession::Entries: a stored turn's speaker or player is not in its observation");

            // What came before the line: the events since the previous turn, the request's events, the player's line.
            std::vector<std::string> before;
            for (const core::ConversationEvent& event : recorded) {
                if (event.afterTurn == previous) {
                    before.push_back("(" + event.event.at("text").get<std::string>() + ")");
                }
            }
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            if (turn.playerText) {
                before.push_back(player->name + ": " + *turn.playerText);
            }

            // The line under the speaker's name, with the requested action in brackets; a refused one shows none.
            std::string line = speaker->name;
            if (turn.adjudication.requested) {
                const nlohmann::json& params = turn.adjudication.params;
                line += params.empty() ? std::format(" [{}]", turn.adjudication.actionId)
                                       : std::format(" [{} {}]", turn.adjudication.actionId,
                                                     params.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            }
            line += ": " + turn.line;

            // The user message holds what came before, one per line, or the cue; the fact is all of it on one line.
            std::string user;
            std::string fact;
            for (const std::string& part : before) {
                user += (user.empty() ? "" : "\n") + part;
                fact += part + " ";
            }
            if (user.empty()) {
                user = FillVariables(HOSTED_TURN_CUE, speaker->name, player->name);
            }
            fact += line;

            context::TranscriptEntry entry;
            entry.turn = turn.number;
            entry.messages = {{inference::Role::User, std::move(user)}, {inference::Role::Assistant, line}};
            entry.foldMessages = entry.messages;
            entry.fact = std::move(fact);
            entries.push_back(std::move(entry));
        }
        previous = turn.number;
    }
    return entries;
}

void HostedSession::SyncHistory()
{
    // Every conversation's folded turns, rendered as entries, are the run's searchable history.
    std::vector<context::TranscriptEntry> folded;
    for (const core::Conversation& conversation : _store.Conversations()) {
        for (context::TranscriptEntry& entry : Entries(conversation, true)) {
            folded.push_back(std::move(entry));
        }
    }
    _index.SyncEntries(folded, _store, *_pBackend);
}

FitPass HostedSession::Pass(std::string_view conversationId, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its folded mark.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    inputs.summaries = _store.Summaries();
    inputs.transcript = Entries(*conversation, false);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}

MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, int turn)
{
    // A fold's summary joins the store with the conversation's folded mark, and the folded turns become history.
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0,
                        [this, conversationId](const core::Summary& summary) {
                            _store.FoldConversation(conversationId, summary);
                            SyncHistory();
                        });
}

} // namespace morpheus::turn
