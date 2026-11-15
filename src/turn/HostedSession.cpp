#include "turn/HostedSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <set>
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
// The task line of the placeholder tail a catch-up (CatchUp) measures an NPC's prompt with.
constexpr std::string_view MAINTENANCE_INSTRUCTION = "(the conversation's next turn)";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
constexpr std::array<store::EmbeddingKind, 3> HOSTED_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                                store::EmbeddingKind::Summary};
// The variables the engine fills in the hosted texts.
constexpr std::string_view SPEAKER_VARIABLE = "{speaker}";
constexpr std::string_view PLAYER_VARIABLE = "{player}";
constexpr std::string_view ACTOR_VARIABLE = "{actor}";

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

// Actor `id`'s name as a hosted prompt shows it: as one of the conversation's `participants`, else as one of
// `observed` (an observation's actors), else the id itself.
std::string ActorName(const std::vector<core::Participant>& participants, const std::vector<core::Actor>& observed,
                      std::string_view id)
{
    for (const core::Participant& participant : participants) {
        if (participant.id == id) {
            return participant.name;
        }
    }
    for (const core::Actor& actor : observed) {
        if (actor.id == id) {
            return actor.name;
        }
    }
    return std::string(id);
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
    // The speaker's memories sit in the tail, capped by summary_cap, so the tail must have room for more than them.
    if (settings.budget.summaryCap >= settings.budget.tailAllowance) {
        throw core::DataError(std::format("settings: summary_cap {} must be below tail_allowance {} for a hosted run: "
                                          "the speaker's memories sit in the tail",
                                          settings.budget.summaryCap, settings.budget.tailAllowance));
    }
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
    // It starts at the observation's game time, after an open rollback is abandoned.
    AbandonRollbackForNewRecord();
    return _store.CreateConversation(participants, observed.clock);
}

void HostedSession::RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    HostedWorld::CheckEvents(events);
    AbandonRollbackForNewRecord();
    // They follow the conversation's latest turn (0 before its first), so they come before its next. With no
    // observation of their own, they are dated with the run's latest seen clock.
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    _store.AddConversationEvents(conversation.id, played.empty() ? 0 : played.back().number, events,
                                 _store.LatestClock());
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
    // The change happens at the observation's game time, after an open rollback is abandoned.
    AbandonRollbackForNewRecord();
    _store.SetParticipants(conversation.id, participants, observed.clock);
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
    // The request passed: an open rollback is abandoned before anything is written, the catch-up's folds included.
    AbandonRollbackForNewRecord();

    // Run-wide, shared with the summaries' ranges, and never reused: one past every turn stored, hidden ones included.
    const int turn = _store.NextHostedTurn();
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

    // The speaker's own memory of this conversation reaches the transcript's cut before it speaks, so nothing it
    // witnessed is in neither its L3 nor its memory.
    if (!CatchUp(conversation.id, request.speakerId, conversation.foldedThrough, turn, stop)) {
        return std::nullopt;
    }

    // 1. Retrieve from lore, the folded turns the speaker witnessed (this and earlier conversations) and its own
    // summaries, with the player text, the speaker's name and the previous line.
    RetrievalQuery query;
    query.input = playerText.value_or("");
    query.scene = {pSpeaker->name};
    query.digest = played.empty() ? std::string() : played.back().line;
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, HOSTED_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend,
                        request.speakerId);

    // 2. Resolve: a prompt that fits (the fit loop sheds hits, condenses and folds), under the proposal grammar.
    context::HostedInputs tail;
    tail.observation = observation.json;
    tail.events = newEvents;
    tail.playerLine = playerText ? player.name + ": " + *playerText : std::string();
    tail.instruction = resolveInstruction;
    // The fit loop sheds retrieved hits first. When the tail is still over tail_allowance with none left, the oldest
    // events leave the prompt one by one until it fits (the store keeps them all; the record counts them). The
    // observation, the player text and the instruction must fit on their own: with no event left, an over-long tail
    // is the fit loop's TurnError, before the resolve call (folds the catch-up and the fit loop finished stay).
    // The speaker's L3 is the conversation's unfolded turns it witnessed, the mark read again each pass (a fold moves
    // it; no turn is committed meanwhile, so `played` stays the conversation's turns); a fold during this turn is the
    // speaker's memory and cuts the shared transcript.
    auto speakerPass = [&](const std::vector<RetrievalHit>& kept) {
        const std::optional<core::Conversation> now = _store.Conversation(conversation.id);
        MORPHEUS_REQUIRE(now.has_value(), "HostedSession::RunTurn: the conversation is not in the store");
        return Pass(conversation.id, request.speakerId,
                    Entries(*now, played, now->foldedThrough, _store.LatestHostedTurn(), request.speakerId), kept,
                    tail);
    };
    MemoryUpkeep upkeep = Upkeep(conversation.id, request.speakerId, observation.json, turn, true);
    std::optional<context::Assembly> assembly = upkeep.Fit(
        hits,
        [&](const std::vector<RetrievalHit>& kept) {
            FitPass pass = speakerPass(kept);
            while (pass.assembly.tailOverBudget && kept.empty() && !tail.events.empty()) {
                tail.events.erase(tail.events.begin());
                pass = speakerPass(kept);
            }
            return pass;
        },
        stop);
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
    record.eventsOmitted = static_cast<int>(newEvents.size() - tail.events.size());
    nlohmann::json timings = nlohmann::json::object();
    timings["resolve"] = CallTelemetry(resolved, resolveSpec);
    if (!adjudication.requested) {
        timings["resolve"]["refused"] = adjudication.reasons; // The log of why the speaker took no action.
    }
    timings["speak"] = CallTelemetry(spoken, speakSpec);
    // The hits that made it into the prompt, and why they matched: the host's lore, the speaker's own memories (by
    // summary id) and the folded turns it witnessed (by turn number).
    timings["retrieval"] = nlohmann::json::array();
    for (const RetrievalHit& hit : hits) {
        const char* source = hit.kind == store::EmbeddingKind::Lore      ? "lore"
                             : hit.kind == store::EmbeddingKind::Summary ? "memory"
                                                                         : "history";
        timings["retrieval"].push_back({{"source", source},
                                        {"ref", hit.refId},
                                        {"score", hit.score},
                                        {"reasons", hit.reasons}});
    }
    record.timings = std::move(timings);
    record.clock = observation.clock; // The game time it was played at.
    _store.CommitHostedTurn(record);
    return record;
}

void HostedSession::EndConversation(std::string_view conversationId)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    AbandonRollbackForNewRecord();
    // With no observation of its own, the end is dated with the run's latest seen clock.
    _store.EndConversation(conversation.id, _store.LatestClock());
}

bool HostedSession::Maintain(std::stop_token stop, std::stop_token yield)
{
    // The pass is made of steps: an ended conversation's mark, one NPC's catch-up in one conversation, one applied
    // share. Each step's writes land as it ends, so a pass that returns early resumes from the marks next time.
    // Between steps the pass returns false when `yield` is requested, but only after a step of its own, so every pass
    // makes progress however often requests arrive; a step in progress finishes.
    bool stepped = false; // This pass finished a step.
    auto yielding = [&stepped, &yield] { return stepped && yield.stop_requested(); };

    // Every NPC catches up in every conversation: an ended conversation is folded through its last turn first, for
    // every NPC witness; an open one through its current cut. Condensing happens in the upkeep, per NPC, when an NPC's
    // memories pass summary_cap. Seeds name the run's latest turn.
    const int latest = _store.LatestHostedTurn();
    for (const core::Conversation& conversation : _store.Conversations()) {
        const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversation.id);
        if (turns.empty()) {
            continue;
        }
        // An ended conversation's remaining turns leave the shared transcript at once and become searchable history;
        // each witness's memory of them is written below.
        int through = conversation.foldedThrough;
        if (conversation.ended && turns.back().number > through) {
            if (yielding()) {
                return false;
            }
            through = turns.back().number;
            _store.MarkFolded(conversation.id, through);
            SyncHistory();
            stepped = true;
        }
        for (const std::string& npc : NpcWitnesses(turns)) {
            // An NPC whose memory already reaches the cut has no step to take.
            if (_store.RememberedThrough(conversation.id, npc) >= through) {
                continue;
            }
            if (yielding()) {
                return false;
            }
            if (!CatchUp(conversation.id, npc, through, latest, stop)) {
                return false;
            }
            stepped = true;
        }
    }

    // Shares waiting for their teller: applied once the conversation ended and the teller caught up, then marked
    // applied at the run's latest seen clock (the row stays).
    // Each applied share may embed its copies, so a stop is honoured before each one, and a yield before each one
    // after a step.
    for (const core::PendingShare& share : _store.PendingShares()) {
        if (stop.stop_requested()) {
            return false;
        }
        // ShareMemories stores a share only for a conversation with a turn its teller witnessed: one without is
        // damaged saved data.
        const std::optional<core::Conversation> conversation = _store.Conversation(share.conversationId);
        const std::vector<core::HostedTurn> turns = _store.HostedTurns(share.conversationId);
        if (!conversation || turns.empty()) {
            throw store::StoreError(std::format("pending share {}: conversation_id '{}' names no conversation with a "
                                                "turn",
                                                share.id, share.conversationId));
        }
        const bool complete = conversation->ended &&
                              _store.RememberedThrough(share.conversationId, share.fromActor) >= turns.back().number;
        if (complete) {
            if (yielding()) {
                return false;
            }
            ApplyShare(share.fromActor, share.toActor, share.conversationId);
            _store.MarkShareApplied(share.id, _store.LatestClock());
            stepped = true;
        }
    }
    return true;
}

ShareResult HostedSession::ShareMemories(const std::string& fromActorId, const std::string& toActorId,
                                         const std::string& conversationId)
{
    // The conversation, the teller (an NPC witness of it) and the listener (another actor).
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    if (!conversation) {
        throw core::DataError(std::format("conversation_id: there is no conversation '{}'", conversationId));
    }
    const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversationId);
    const std::vector<std::string> npcs = NpcWitnesses(turns);
    if (std::find(npcs.begin(), npcs.end(), fromActorId) == npcs.end()) {
        throw core::DataError(std::format("from: '{}' witnessed no turn of conversation '{}' as an NPC", fromActorId,
                                          conversationId));
    }
    if (toActorId.empty() || toActorId == fromActorId) {
        throw core::DataError(std::format("to: '{}' must be another actor than the teller '{}'", toActorId, fromActorId));
    }
    // Only NPCs keep memories: the listener may not be the player in any of the conversation's turns. An id the turns
    // never show is allowed (an NPC who has not spoken or listened yet).
    for (const core::HostedTurn& turn : turns) {
        const std::vector<core::Actor> actors = HostedWorld::ActorsOf(turn.observation);
        const bool isPlayer = std::any_of(actors.begin(), actors.end(), [&toActorId](const core::Actor& actor) {
            return actor.id == toActorId && actor.isPlayer;
        });
        if (isPlayer) {
            throw core::DataError(std::format("to: '{}' is the player; only NPCs keep memories", toActorId));
        }
    }
    // The share passed its checks: an open rollback is abandoned before it stores a pending share or copies.
    AbandonRollbackForNewRecord();

    // The teller's memory of it is complete once the conversation ended and the teller caught up to its last turn;
    // until then the share waits, stored once however often it is asked for. The teller witnessed a turn, so the
    // conversation has one.
    const bool complete =
        conversation->ended && _store.RememberedThrough(conversationId, fromActorId) >= turns.back().number;
    if (!complete) {
        const std::vector<core::PendingShare> pending = _store.PendingShares();
        const bool stored = std::any_of(pending.begin(), pending.end(), [&](const core::PendingShare& share) {
            return share.fromActor == fromActorId && share.toActor == toActorId &&
                   share.conversationId == conversationId;
        });
        if (!stored) {
            // Created at the run's latest seen clock.
            _store.AddPendingShare({0, fromActorId, toActorId, conversationId, _store.LatestClock()});
        }
        return {0, true};
    }
    return {ApplyShare(fromActorId, toActorId, conversationId), false};
}

RewindResult HostedSession::Rewind(double gameTime)
{
    if (!_world.HasClock()) {
        throw core::DataError("game_time: the host data names no clock, so the run cannot be rewound");
    }
    if (!std::isfinite(gameTime)) {
        throw core::DataError("game_time: expected a finite number");
    }
    // The same time as the open rollback: nothing to do.
    const std::optional<store::Rollback> open = _store.OpenRollback();
    if (open && open->gameTime == gameTime) {
        return {false, 0, 0, gameTime};
    }
    // Undo the open rollback, then apply one when the time is before the latest live record.
    RewindResult result;
    if (open) {
        result.restoredTurns = _store.UndoRollback().restoredTurns;
    }
    const std::optional<double> latest = _store.LatestClock();
    if (latest && gameTime < *latest) {
        result.hiddenTurns = _store.ApplyRollback(gameTime).hiddenTurns;
        result.rollbackTime = gameTime;
    }
    // The host sees the net change. Both rollbacks hide every turn clocked after their time, so one of the two sets
    // holds the other: a turn restored by the undo and hidden again by the new rollback counts as neither.
    const int both = std::min(result.restoredTurns, result.hiddenTurns);
    result.restoredTurns -= both;
    result.hiddenTurns -= both;
    // Rewound: the open rollback changed (one was undone, applied, or both; the same time returned early above).
    result.rewound = open.has_value() || result.rollbackTime.has_value();
    // Turns, folded marks and summaries all changed: the searchable history and memories are indexed again.
    SyncHistory();
    return result;
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

std::vector<core::Summary> HostedSession::Memories(std::string_view actorId)
{
    return _store.ActorSummaries(actorId, true);
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

void HostedSession::AbandonRollbackForNewRecord()
{
    // Playing on after a rewind: what the open rollback hid stays hidden for good, and the new record is written live.
    if (_store.OpenRollback()) {
        _store.AbandonRollback();
    }
}

std::vector<context::TranscriptEntry> HostedSession::Entries(const core::Conversation& conversation,
                                                             const std::vector<core::HostedTurn>& turns, int afterTurn,
                                                             int throughTurn, std::string_view witnessId)
{
    const std::vector<core::ConversationEvent> recorded = _store.ConversationEvents(conversation.id);
    std::vector<context::TranscriptEntry> entries;
    // `previous` follows every turn of the conversation, rendered or not: events are placed by the conversation's
    // turns, whoever witnessed them.
    int previous = 0; // The conversation's turn before the one rendered; 0 before its first.
    for (const core::HostedTurn& turn : turns) {
        // A turn is rendered when it is in the range and, for a witness, its speaker or a listener was that witness.
        const bool witnessed = witnessId.empty() || turn.speakerId == witnessId ||
                               std::find(turn.listenerIds.begin(), turn.listenerIds.end(), witnessId) !=
                                   turn.listenerIds.end();
        if (turn.number > afterTurn && turn.number <= throughTurn && witnessed) {
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
            // The events its prompt left out (the oldest, when the tail was over tail_allowance) stay out of its
            // entry too, so one long burst of events does not overflow L3 later.
            // RunStore checked the count is not negative; one over what came before the turn is a damaged file.
            if (turn.eventsOmitted > std::ssize(before)) {
                throw store::StoreError(std::format("turn {}: events_omitted {} is more than the {} events before it",
                                                    turn.number, turn.eventsOmitted, std::ssize(before)));
            }
            before.erase(before.begin(), before.begin() + turn.eventsOmitted);
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
    // Every conversation's folded turns, rendered as entries with their witnesses (speaker and listeners), are the
    // run's searchable history. Each conversation's turns are read once, for both.
    std::vector<WitnessedEntry> folded;
    for (const core::Conversation& conversation : _store.Conversations()) {
        const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversation.id);
        std::map<int, std::vector<std::string>> witnesses; // Turn number -> its speaker and listeners.
        for (const core::HostedTurn& turn : turns) {
            std::vector<std::string>& ids = witnesses[turn.number];
            ids.push_back(turn.speakerId);
            ids.insert(ids.end(), turn.listenerIds.begin(), turn.listenerIds.end());
        }
        for (context::TranscriptEntry& entry : Entries(conversation, turns, 0, conversation.foldedThrough, "")) {
            std::vector<std::string> ids = witnesses.at(entry.turn);
            folded.push_back({std::move(entry), std::move(ids)});
        }
    }
    _index.SyncEntries(folded, _store, *_pBackend);
    // The NPC summaries are a second source, each owned by its NPC.
    SyncMemories();
}

void HostedSession::SyncMemories()
{
    _index.SyncSummaries(_store.HostedSummaries(), _store, *_pBackend);
}

FitPass HostedSession::Pass(std::string_view conversationId, std::string_view actorId,
                            std::vector<context::TranscriptEntry> transcript, const std::vector<RetrievalHit>& hits,
                            context::HostedInputs inputs)
{
    // The conversation as the store holds it now: a fold since the last pass moved its marks.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Pass: the conversation is not in the store");
    // Both callers pass a checked observation (the request's, or the last stored turn's): names come from it too.
    MORPHEUS_REQUIRE(inputs.observation.is_object(), "HostedSession::Pass: the tail needs its observation");
    const std::vector<core::Actor> observed = HostedWorld::ActorsOf(inputs.observation);
    inputs.systemText = std::string(HOSTED_SYSTEM_TEXT);
    inputs.lore = _world.Lore();
    inputs.participants = conversation->participants;
    // The speaker's shown summaries are its memories, oldest first; a shared one names its teller.
    inputs.speakerName = ActorName(conversation->participants, observed, actorId);
    inputs.memories.clear();
    for (const core::Summary& summary : _store.ActorSummaries(actorId, false)) {
        inputs.memories.push_back(
            {summary.toldBy.empty() ? std::string() : ActorName(conversation->participants, observed, summary.toldBy),
             summary.text});
    }
    inputs.transcript = std::move(transcript);
    inputs.retrieved.clear();
    for (const RetrievalHit& hit : hits) {
        inputs.retrieved.push_back(hit.text);
    }

    FitPass pass;
    pass.assembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
    pass.transcript = std::move(inputs.transcript);
    return pass;
}

MemoryUpkeep HostedSession::Upkeep(const std::string& conversationId, const std::string& actorId,
                                   const nlohmann::json& observation, int turn, bool foldsConversation)
{
    // Names as the actor's prompt shows them (Pass): a participant of the conversation, else an actor of the
    // observation, else the id.
    const std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    MORPHEUS_REQUIRE(conversation.has_value(), "HostedSession::Upkeep: the conversation is not in the store");
    MORPHEUS_REQUIRE(observation.is_object(), "HostedSession::Upkeep: names need the observation");
    std::vector<core::Participant> participants = conversation->participants;
    std::vector<core::Actor> observed = HostedWorld::ActorsOf(observation);
    const std::string name = ActorName(participants, observed, actorId);
    // {actor} in a hosted text is the actor's name. The scan goes on after each name it puts in, so a name that holds
    // {actor} itself is put in once.
    auto fill = [&name](std::string_view text) {
        std::string filled(text);
        for (std::size_t at = filled.find(ACTOR_VARIABLE); at != std::string::npos;
             at = filled.find(ACTOR_VARIABLE, at + name.size())) {
            filled.replace(at, ACTOR_VARIABLE.size(), name);
        }
        return filled;
    };

    // The actor's scope: its shown memories, with the hosted fold and condense texts naming it.
    SummaryScope scope;
    scope.shown = [this, actorId] { return _store.ActorSummaries(actorId, false); };
    scope.storeFold = [this, conversationId, actorId, foldsConversation](const core::Summary& summary) {
        // The actor's memory of the conversation; a fold during its turn also cuts the shared transcript there.
        core::Summary memory = summary;
        memory.actorId = actorId;
        memory.conversationId = conversationId;
        // The summary becomes a searchable memory. A fold during the actor's turn also moved the conversation's
        // folded_through, so its folded turns become searchable history; a catch-up only writes a memory of turns
        // that are history already.
        if (foldsConversation) {
            _store.FoldConversation(conversationId, memory);
            SyncHistory();
        } else {
            _store.RememberFold(memory);
            SyncMemories();
        }
    };
    // A condense supersedes the actor's shown memories with one condensed memory of no conversation.
    scope.storeCondensed = [this, actorId](const core::Summary& condensed) {
        core::Summary memory = condensed;
        memory.actorId = actorId;
        _store.CondenseActorSummaries(actorId, memory);
        SyncMemories(); // Only memories changed.
    };
    scope.foldInstruction = fill(HOSTED_SUMMARIZE_INSTRUCTION);
    // The condense message outlives this call, so it holds its own copies of the names it needs and of the filled
    // instruction (`fill` refers to the local name).
    scope.condenseMessage = [name, participants = std::move(participants), observed = std::move(observed),
                             instruction = fill(HOSTED_CONDENSE_INSTRUCTION)](
                                const std::vector<core::Summary>& summaries) {
        // The memories as the actor's prompt shows them, tellers by name, then the instruction.
        std::vector<context::MemoryLine> lines;
        for (const core::Summary& summary : summaries) {
            lines.push_back(
                {summary.toldBy.empty() ? std::string() : ActorName(participants, observed, summary.toldBy),
                 summary.text});
        }
        return context::RenderMemories(name, lines) + "\n" + instruction;
    };
    return MemoryUpkeep(_store, *_pBackend, _settings, std::string(HOSTED_TAIL_NAME), turn, 0, std::move(scope));
}

bool HostedSession::CatchUp(const std::string& conversationId, const std::string& actorId, int throughTurn,
                            int seedTurn, std::stop_token stop)
{
    // Nothing to do when the actor's memory already reaches throughTurn.
    if (_store.RememberedThrough(conversationId, actorId) >= throughTurn) {
        return true;
    }
    // The actor's witnessed turns after its mark through throughTurn fold into its memory (FoldAll: in one call when
    // they fit), each pass reading the mark again: a fold moves it. The conversation's last observation names the
    // actors, and a placeholder task stands in for the tail, which no fold reads.
    const std::vector<core::HostedTurn> turns = _store.HostedTurns(conversationId);
    const nlohmann::json observation = turns.empty() ? nlohmann::json::object() : turns.back().observation;
    auto assemble = [&](const std::vector<RetrievalHit>&) {
        const std::optional<core::Conversation> now = _store.Conversation(conversationId);
        MORPHEUS_REQUIRE(now.has_value(), "HostedSession::CatchUp: the conversation is not in the store");
        context::HostedInputs inputs;
        inputs.observation = observation;
        inputs.instruction = std::string(MAINTENANCE_INSTRUCTION);
        return Pass(conversationId, actorId,
                    Entries(*now, turns, _store.RememberedThrough(conversationId, actorId), throughTurn, actorId), {},
                    inputs);
    };
    // Each fold's summary is the actor's and moves only its own mark (a catch-up); its turns are indexed as it lands.
    if (!Upkeep(conversationId, actorId, observation, seedTurn, false).FoldAll(assemble, stop)) {
        return false;
    }
    // Turns it did not witness need no memory: its mark reaches throughTurn.
    if (_store.RememberedThrough(conversationId, actorId) < throughTurn) {
        _store.SetRememberedThrough(conversationId, actorId, throughTurn);
    }
    return true;
}

int HostedSession::ApplyShare(const std::string& fromActorId, const std::string& toActorId,
                              const std::string& conversationId)
{
    // What the listener already has from these summaries: copies are keyed by their source.
    std::set<std::int64_t> copied;
    for (const core::Summary& summary : _store.ActorSummaries(toActorId, true)) {
        if (summary.sourceId != 0) {
            copied.insert(summary.sourceId);
        }
    }
    // The teller's own summaries of the conversation (what it saw, not what it was told), as snapshots, oldest first,
    // each copy dated with the run's latest seen clock.
    const std::optional<double> clock = _store.LatestClock();
    int shared = 0;
    for (const core::Summary& summary : _store.ActorSummaries(fromActorId, true)) {
        if (summary.conversationId != conversationId || !summary.toldBy.empty() || copied.contains(summary.id)) {
            continue;
        }
        core::Summary copy;
        copy.fromTurn = summary.fromTurn;
        copy.toTurn = summary.toTurn;
        copy.text = summary.text;
        copy.actorId = toActorId;
        copy.conversationId = conversationId;
        copy.toldBy = fromActorId;
        copy.sourceId = summary.id;
        copy.clock = clock;
        _store.AddActorSummary(copy);
        ++shared;
    }
    if (shared > 0) {
        SyncMemories(); // The copies are searchable by their new owner.
    }
    return shared;
}

std::vector<std::string> HostedSession::NpcWitnesses(const std::vector<core::HostedTurn>& turns)
{
    // Who spoke or listened, as an NPC: a turn's player is in its observation.
    std::vector<std::string> npcs;
    auto add = [&npcs](const std::string& id) {
        if (std::find(npcs.begin(), npcs.end(), id) == npcs.end()) {
            npcs.push_back(id);
        }
    };
    for (const core::HostedTurn& turn : turns) {
        const std::vector<core::Actor> actors = HostedWorld::ActorsOf(turn.observation);
        auto isPlayer = [&actors](const std::string& id) {
            return std::any_of(actors.begin(), actors.end(),
                               [&id](const core::Actor& actor) { return actor.id == id && actor.isPlayer; });
        };
        // The speaker is never the player (RunTurn refuses that).
        add(turn.speakerId);
        for (const std::string& listener : turn.listenerIds) {
            if (!isPlayer(listener)) {
                add(listener);
            }
        }
    }
    return npcs;
}

} // namespace morpheus::turn
