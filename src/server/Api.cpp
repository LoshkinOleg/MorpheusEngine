#include "server/Api.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/SentenceSplitter.h"
#include "server/SlotBackend.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"
#include "turn/PromptPack.h"

namespace morpheus::server {

namespace {

// ---- Constants

// How long the handler waits for a turn's first event before it checks that the worker took the turn at all.
constexpr std::chrono::milliseconds START_POLL = std::chrono::milliseconds(100);

// A request for something that does not exist: an unknown route, or an unknown conversation of an opened run (404).
class NotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A run used before its open in this process: its host data comes with the open (409).
class NotOpened : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The status of a request that failed with `error` (spec, section 1, "Errors").
int StatusOf(const std::exception& error)
{
    if (dynamic_cast<const NotFound*>(&error) != nullptr) {
        return 404;
    }
    if (dynamic_cast<const NotOpened*>(&error) != nullptr) {
        return 409;
    }
    if (dynamic_cast<const core::DataError*>(&error) != nullptr) {
        return 400;
    }
    if (dynamic_cast<const turn::TurnError*>(&error) != nullptr) {
        return 422;
    }
    if (dynamic_cast<const inference::InferenceError*>(&error) != nullptr) {
        return 503;
    }
    return 500;
}

// Conversation `id`, which must exist in the run (NotFound otherwise); an ended one is HostedSession's DataError.
void RequireConversation(turn::HostedSession& session, std::string_view id)
{
    if (!session.Conversation(id)) {
        throw NotFound(std::format("conversation_id: there is no conversation '{}'", id));
    }
}

// An adjudication's action as the host reads it: {"id", "params"}, or null for a refused one.
nlohmann::json ActionJson(const core::Adjudication& adjudication)
{
    if (!adjudication.requested) {
        return nullptr;
    }
    return {{"id", adjudication.actionId}, {"params", adjudication.params}};
}

// A conversation's key in the slot pool: its run's id and its own.
std::string SlotKey(std::string_view runId, std::string_view conversationId)
{
    return std::format("{}/{}", runId, conversationId);
}

// The body's string `key`. DataError "<key>: ..." when it is missing or not a string.
std::string RequiredString(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) {
        throw core::DataError(std::format("{}: expected a string", key));
    }
    return it->get<std::string>();
}

// The body's array of strings `key`. DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<std::string> StringList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of strings", key));
    }
    std::vector<std::string> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_string()) {
            throw core::DataError(std::format("{}/{}: expected a string", key, i));
        }
        list.push_back((*it)[i].get<std::string>());
    }
    return list;
}

// The body's array of objects `key` (events). DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<nlohmann::json> ObjectList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of objects", key));
    }
    std::vector<nlohmann::json> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_object()) {
            throw core::DataError(std::format("{}/{}: expected an object", key, i));
        }
        list.push_back((*it)[i]);
    }
    return list;
}

// The body's object `key` (an observation). DataError "<key>: ..." when it is missing or not an object.
const nlohmann::json& RequiredObject(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_object()) {
        throw core::DataError(std::format("{}: expected an object", key));
    }
    return *it;
}

} // namespace

nlohmann::json ErrorBody(int status, std::string_view message)
{
    return {{"api_version", std::string(API_VERSION)}, {"error", {{"status", status}, {"message", message}}}};
}

Api::Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log)
    : _pRuns(&runs)
    , _pSlots(&slots)
    , _pUtility(&utility)
    , _health(std::move(health))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_log), "Api: a log is needed");
}

ApiResponse Api::Handle(std::string_view method, std::string_view path, std::string_view body)
{
    ApiResponse response;
    try {
        // The body: a JSON object, or nothing (an empty object).
        nlohmann::json json = nlohmann::json::object();
        if (!body.empty()) {
            json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                throw core::DataError("body: expected a JSON object");
            }
        }

        // The path's segments: "/v1/runs/save_1/open" is {"v1", "runs", "save_1", "open"}.
        std::vector<std::string_view> parts;
        for (std::size_t start = 0; start < path.size();) {
            const std::size_t end = std::min(path.find('/', start), path.size());
            if (end > start) {
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1;
        }
        const bool get = method == "GET";
        const bool post = method == "POST";
        auto route = [&parts](std::initializer_list<std::string_view> shape) {
            return std::equal(parts.begin(), parts.end(), shape.begin(), shape.end());
        };
        auto unknown = [&] { return NotFound(std::format("there is no route {} {}", method, path)); };

        if (get && route({"health"})) {
            response.body = {{"ready", true},
                             {"chat_model", _health.chatModel},
                             {"embedding_model", _health.embeddingModel},
                             {"slots", _health.slots}};
        } else if (get && route({"v1", "prompts", "default"})) {
            // The engine's default texts and the variables each takes, for a host writing its own pack.
            const turn::PromptPack defaults;
            nlohmann::json prompts = nlohmann::json::object();
            nlohmann::json variables = nlohmann::json::object();
            for (const turn::PromptText& text : turn::PROMPT_TEXTS) {
                const std::string key(text.key);
                prompts[key] = defaults.Text(text.key);
                variables[key] = nlohmann::json::array();
                for (const std::string_view name : text.variables) {
                    if (!name.empty()) {
                        variables[key].push_back(std::string(name));
                    }
                }
            }
            response.body = {{"prompts", std::move(prompts)}, {"variables", std::move(variables)}};
        } else if (post && route({"v1", "tokenize"})) {
            // Exact counts from the chat model, for the host's own budgeting.
            const std::string content = RequiredString(json, "content");
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"tokens", _pUtility->CountTokens(content)}};
        } else if (post && route({"v1", "embeddings"})) {
            const std::vector<std::string> input = StringList(json, "input");
            const std::string purpose = RequiredString(json, "purpose");
            if (purpose != "query" && purpose != "document") {
                throw core::DataError("purpose: expected \"query\" or \"document\"");
            }
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"embeddings", _pUtility->Embed(input, purpose == "query" ? inference::EmbedPurpose::Query
                                                                                       : inference::EmbedPurpose::Document)},
                             {"model", _pUtility->EmbeddingModelId()}};
        } else if (parts.size() >= 4 && parts[0] == "v1" && parts[1] == "runs") {
            const std::string runId(parts[2]);
            RunRegistry::CheckRunId(runId);
            // The run's worker; a run not opened in this process has none (its host data comes with the open).
            auto worker = [this, &runId]() -> RunWorker& {
                RunWorker* pWorker = _pRuns->Find(runId);
                if (pWorker == nullptr) {
                    throw NotOpened(std::format("run '{}' is not open: open it first (POST /v1/runs/{}/open)", runId, runId));
                }
                return *pWorker;
            };

            if (post && parts.size() == 4 && parts[3] == "open") {
                // The host data, and an optional seed for a new run's sampling.
                std::uint64_t seed = 0;
                if (json.contains("seed")) {
                    if (!json.at("seed").is_number_unsigned()) {
                        throw core::DataError("seed: expected an integer of at least 0");
                    }
                    seed = json.at("seed").get<std::uint64_t>();
                    json.erase("seed");
                }
                const bool created = _pRuns->Open(runId, turn::HostDataFromJson(json), seed);
                response.body = {{"run_id", runId}, {"created", created}};
            } else if (post && parts.size() == 5 && parts[3] == "memories" && parts[4] == "share") {
                // One NPC tells another what it remembers of a conversation (per-speaker memory spec, section 2).
                const std::string from = RequiredString(json, "from");
                const std::string to = RequiredString(json, "to");
                const std::string conversationId = RequiredString(json, "conversation_id");
                turn::ShareResult result;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    RequireConversation(session, conversationId);
                    result = session.ShareMemories(from, to, conversationId);
                });
                response.body = {{"shared", result.shared}, {"pending", result.pending}};
            } else if (get && parts.size() == 6 && parts[3] == "actors" && parts[5] == "memories") {
                // Every memory of an actor, oldest first, for debugging and the host's own tests.
                const std::string actorId(parts[4]);
                nlohmann::json memories = nlohmann::json::array();
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    for (const core::Summary& memory : session.Memories(actorId)) {
                        memories.push_back({{"conversation_id", memory.conversationId},
                                            {"from_turn", memory.fromTurn},
                                            {"to_turn", memory.toTurn},
                                            {"text", memory.text},
                                            {"told_by", memory.toldBy},
                                            {"superseded", memory.superseded}});
                    }
                });
                response.body = {{"memories", memories}};
            } else if (post && parts.size() == 4 && parts[3] == "rewind") {
                // The run as it was at a save's game time (rollback spec, section 3).
                const auto field = json.find("game_time");
                if (field == json.end() || !field->is_number()) {
                    throw core::DataError("game_time: expected a number");
                }
                const double gameTime = field->get<double>();
                turn::RewindResult result;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    result = session.Rewind(gameTime);
                });
                response.body = {{"rewound", result.rewound},
                                 {"hidden_turns", result.hiddenTurns},
                                 {"restored_turns", result.restoredTurns},
                                 {"rollback", result.rollbackTime ? nlohmann::json{{"game_time", *result.rollbackTime}}
                                                                  : nlohmann::json(nullptr)}};
            } else if (post && parts.size() == 4 && parts[3] == "conversations") {
                const std::vector<std::string> participants = StringList(json, "participants");
                const nlohmann::json& observation = RequiredObject(json, "observation");
                std::string id;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    id = session.StartConversation(participants, observation);
                });
                response.body = {{"conversation_id", id}};
            } else if (parts.size() >= 5 && parts[3] == "conversations") {
                const std::string conversationId(parts[4]);
                if (get && parts.size() == 5) {
                    // The stored records, for debugging and the host's own tests.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        const core::Conversation conversation = *session.Conversation(conversationId);
                        nlohmann::json participants = nlohmann::json::array();
                        for (const core::Participant& participant : conversation.participants) {
                            participants.push_back(
                                {{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
                        }
                        nlohmann::json turns = nlohmann::json::array();
                        for (const core::HostedTurn& turn : session.Turns(conversationId)) {
                            nlohmann::json entry = {
                                {"number", turn.number},
                                {"speaker", turn.speakerId},
                                {"listeners", turn.listenerIds},
                                {"player_text", turn.playerText ? nlohmann::json(*turn.playerText) : nlohmann::json(nullptr)},
                                {"events", turn.events},
                                {"observation", turn.observation},
                                {"resolve_output", turn.resolveOutput},
                                {"action", ActionJson(turn.adjudication)},
                                {"line", turn.line},
                                {"events_omitted", turn.eventsOmitted},
                                {"timings", turn.timings}};
                            if (!turn.adjudication.requested) {
                                entry["refused"] = turn.adjudication.reasons;
                            }
                            turns.push_back(std::move(entry));
                        }
                        nlohmann::json events = nlohmann::json::array();
                        for (const core::ConversationEvent& event : session.Events(conversationId)) {
                            events.push_back({{"after_turn", event.afterTurn}, {"event", event.event}});
                        }
                        response.body = {{"conversation",
                                          {{"id", conversation.id},
                                           {"participants", participants},
                                           {"ended", conversation.ended},
                                           {"folded_through", conversation.foldedThrough}}},
                                         {"turns", turns},
                                         {"events", events}};
                    });
                } else if (post && parts.size() == 6 && parts[5] == "turn") {
                    // The request, checked for its shape here and for its content by HostedSession on the run's worker.
                    turn::HostedTurnRequest request;
                    request.conversationId = conversationId;
                    request.speakerId = RequiredString(json, "speaker");
                    if (json.contains("player_text")) {
                        if (!json.at("player_text").is_string()) {
                            throw core::DataError("player_text: expected a string");
                        }
                        request.playerText = json.at("player_text").get<std::string>();
                    }
                    if (json.contains("events")) {
                        request.events = ObjectList(json, "events");
                    }
                    request.observation = RequiredObject(json, "observation");
                    if (json.contains("allowed_actions")) {
                        request.allowedActions = StringList(json, "allowed_actions");
                    }
                    if (json.contains("review")) {
                        if (!json.at("review").is_boolean()) {
                            throw core::DataError("review: expected true or false");
                        }
                        request.review = json.at("review").get<bool>();
                    }

                    // The turn runs on the run's worker, on its conversation's slot; its events come back through the
                    // stream. It is the only job that stops a running background fold: other requests wait for the
                    // fold's pass, so polling cannot keep folding from finishing. The stream's stop token stops it when the client leaves (TurnStream::Cancel) or the worker
                    // stops. The job logs its own 500s: one after the action reaches no handler, which may have
                    // returned by then, so the job keeps copies of the log and of the route's name.
                    auto stream = std::make_shared<TurnStream>();
                    std::future<void> taken = worker().Post(
                        [stream, request = std::move(request), slotKey = SlotKey(runId, conversationId),
                         pSlots = _pSlots, log = _log, routeName = std::format("{} {}", method, path)](
                            turn::HostedSession& session, SlotBackend& backend, std::stop_token workerStop) {
                            // The stream ends on every exit of the job, an escaping exception's included, so the
                            // handler or the writer reading it never waits forever.
                            struct EndStream {
                                TurnStream* _pStream = nullptr;
                                ~EndStream()
                                {
                                    _pStream->End();
                                }
                            } endStream{stream.get()};
                            std::stop_callback stopWithWorker(workerStop, [&stream] { stream->Cancel(); });
                            try {
                                RequireConversation(session, request.conversationId);
                                backend.SetSlot(pSlots->Acquire(slotKey));
                                SentenceSplitter sentences;
                                turn::HostedCallbacks callbacks;
                                // The turn's speaker as the session reports it: the named one, or the NPC a "decide"
                                // turn chose.
                                std::string speakerId = request.speakerId;
                                callbacks.onSpeaker = [&speakerId](const std::string& id) { speakerId = id; };
                                callbacks.onAction = [&](const core::Adjudication& adjudication) {
                                    nlohmann::json action = {{"api_version", std::string(API_VERSION)},
                                                             {"speaker", speakerId},
                                                             {"action", ActionJson(adjudication)}};
                                    if (!adjudication.requested) {
                                        action["refused"] = adjudication.reasons;
                                    }
                                    stream->Push({"action", std::move(action)});
                                };
                                callbacks.onToken = [&](std::string_view piece) {
                                    // Each piece goes out as it comes, and each sentence as soon as a piece ends it.
                                    stream->Push({"token", {{"text", std::string(piece)}}});
                                    for (std::string& sentence : sentences.Add(piece)) {
                                        stream->Push({"sentence", {{"text", std::move(sentence)}}});
                                    }
                                };
                                const std::optional<core::HostedTurn> record =
                                    session.RunTurn(request, callbacks, stream->StopToken());
                                // A cancelled turn committed nothing and ends with no done. Stopped by the server's
                                // shutdown, it ends with an error the client can read; a client that left reads
                                // nothing, so its cancel stays silent.
                                if (!record && workerStop.stop_requested()) {
                                    stream->Push({"error", {{"status", 503}, {"message", "the server is stopping"}}});
                                }
                                if (record) {
                                    // The line's last sentence has no whitespace after it: it ends with the line.
                                    const std::string rest = sentences.Finish();
                                    if (!rest.empty()) {
                                        stream->Push({"sentence", {{"text", rest}}});
                                    }
                                    // The timings: resolve and speak, and decide, review and respeak when those calls ran.
                                    nlohmann::json timings = {{"resolve", record->timings.at("resolve")},
                                                              {"speak", record->timings.at("speak")}};
                                    if (record->timings.contains("decide")) {
                                        timings["decide"] = record->timings.at("decide");
                                    }
                                    if (record->timings.contains("review")) {
                                        timings["review"] = record->timings.at("review");
                                    }
                                    if (record->timings.contains("respeak")) {
                                        timings["respeak"] = record->timings.at("respeak");
                                    }
                                    stream->Push({"done",
                                                  {{"speaker", record->speakerId},
                                                   {"action", ActionJson(record->adjudication)},
                                                   {"line", record->line},
                                                   {"turn", record->number},
                                                   {"events_omitted", record->eventsOmitted},
                                                   {"timings", timings}}});
                                }
                            } catch (const std::exception& error) {
                                // Before the action, the handler answers with this error's status; after it, the
                                // stream ends with it. A 500 is a bug or damaged data, so the operator sees it too.
                                const int status = StatusOf(error);
                                if (status == 500) {
                                    log(std::format("{} failed: {}", routeName, error.what()));
                                }
                                stream->Push({"error", {{"status", status}, {"message", error.what()}}});
                            }
                        },
                        RunWorker::Preempt::Fold);

                    // The status is known with the first event: an error before the action is that error's JSON
                    // answer, as for any other request (the job logged it if it is a 500); otherwise the stream starts
                    // with the action. A turn the worker refused (it is stopping) never runs: its future says so.
                    while (!stream->WaitStarted(START_POLL)) {
                        if (taken.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                            taken.get(); // Rethrows the refusal: InferenceError, the server is stopping.
                        }
                    }
                    const std::optional<StreamEvent> first = stream->Front();
                    if (!first) {
                        throw inference::InferenceError("the turn was stopped before it began: the server is stopping");
                    }
                    if (first->name == "error") {
                        const int status = first->data.at("status").get<int>();
                        response = ApiResponse{status, ErrorBody(status, first->data.at("message").get<std::string>()),
                                               nullptr};
                    } else {
                        response.stream = stream;
                    }
                } else if (post && parts.size() == 6 && parts[5] == "events") {
                    const std::vector<nlohmann::json> events = ObjectList(json, "events");
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.RecordEvents(conversationId, events);
                    });
                } else if (post && parts.size() == 6 && parts[5] == "participants") {
                    const std::vector<std::string> joined =
                        json.contains("joined") ? StringList(json, "joined") : std::vector<std::string>();
                    const std::vector<std::string> left =
                        json.contains("left") ? StringList(json, "left") : std::vector<std::string>();
                    const nlohmann::json& observation = RequiredObject(json, "observation");
                    nlohmann::json ids = nlohmann::json::array();
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.SetParticipants(conversationId, joined, left, observation);
                        // Kept in a local: a range-for over a member of the returned optional would outlive it.
                        const core::Conversation conversation = *session.Conversation(conversationId);
                        for (const core::Participant& participant : conversation.participants) {
                            ids.push_back(participant.id);
                        }
                    });
                    response.body = {{"participants", ids}};
                } else if (post && parts.size() == 6 && parts[5] == "end") {
                    // The conversation ends; its slot is free for the next one, and folding follows in the background.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.EndConversation(conversationId);
                    });
                    _pSlots->Release(SlotKey(runId, conversationId));
                } else {
                    throw unknown();
                }
            } else {
                throw unknown();
            }
        } else {
            throw unknown();
        }
    } catch (const std::exception& error) {
        // Every failure is its status and message; a 500 is a bug or damaged data, so the operator sees it too.
        const int status = StatusOf(error);
        if (status == 500) {
            _log(std::format("{} {} failed: {}", method, path, error.what()));
        }
        response = ApiResponse{status, ErrorBody(status, error.what()), nullptr};
    }
    if (!response.stream) {
        response.body["api_version"] = std::string(API_VERSION);
    }
    return response;
}

nlohmann::json Api::EscapedError(std::string_view method, std::string_view path, std::string_view what)
{
    // Handle catches every std exception, so what escapes is a bug or not a std exception: the operator sees it.
    _log(std::format("{} {} failed: {}", method, path, what));
    return ErrorBody(500, what);
}

} // namespace morpheus::server
