#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

// The participant ids of conversation `id`, in join order.
std::vector<std::string> ParticipantIds(turn::HostedSession& session, const std::string& id)
{
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    std::vector<std::string> ids;
    for (const core::Participant& participant : conversation->participants) {
        ids.push_back(participant.id);
    }
    return ids;
}

} // namespace

TEST_CASE("Open creates a hosted run with its host data and indexes the retrievable lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384); // No model call is made: no responder.
    const std::filesystem::path file = test::HostedRunFile("hosted_open");
    const turn::HostData data = test::FixtureHostData();
    turn::HostedSession session = turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend);
    CHECK(session.Meta().kind == store::RunKind::Hosted);
    CHECK(session.Meta().worldId == "hosted");
    CHECK(session.Meta().worldHash == turn::HostDataHash(data));
    CHECK(session.Meta().rngSeed == 42u);
    CHECK(session.Meta().modelId == "chat");
    CHECK(backend.EmbeddedTexts() == 2); // Hulda and Saadia; Whiterun and the inn are always on.
    CHECK(session.Memories(test::HULDA).empty());
    CHECK(std::filesystem::exists(file));
}

TEST_CASE("Open with the same host data reopens the run as it is", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_reopen");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    }
    const int embedded = backend.EmbeddedTexts();
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 7, backend);
    CHECK(backend.EmbeddedTexts() == embedded); // The stored vectors are reused.
    CHECK(reopened.Meta().rngSeed == 42u);      // The run's seed, not the call's.
    CHECK(ParticipantIds(reopened, id) == std::vector<std::string>{test::HULDA, test::AELA});
}

TEST_CASE("Open with changed host data replaces the stored copy and re-embeds the changed lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_changed");
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
    }
    // A mod update: one lore entry and the catalog change.
    turn::HostData changed = test::FixtureHostData();
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    changed.catalog["actions"].push_back(nlohmann::json{{"id", "trade"}});
    const int embedded = backend.EmbeddedTexts();
    {
        turn::HostedSession reopened = turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, backend);
        CHECK(reopened.Meta().worldHash == turn::HostDataHash(changed));
        CHECK(backend.EmbeddedTexts() == embedded + 1); // Only Hulda's entry is embedded again.
    }
    store::RunStore stored = store::RunStore::Open(file);
    CHECK(stored.HostData() == turn::HostDataToJson(changed));
    CHECK(stored.Meta().worldHash == turn::HostDataHash(changed));
}

TEST_CASE("A reopen whose changed lore cannot be embedded leaves the stored host data and its hash unchanged",
          "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_reopen_embed_fails");
    const turn::HostData original = test::FixtureHostData();
    {
        test::ScriptedBackend backend(16384);
        turn::HostedSession session = turn::HostedSession::Open(file, original, test::HostedSettings(), 42, backend);
    }

    // A mod update whose changed lore entry the embedding model refuses: Embed throws during the re-embed.
    turn::HostData changed = test::FixtureHostData();
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    test::ScriptedBackend small(16384);
    small.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, small), core::DataError,
                         MessageMatches(ContainsSubstring("host data: lore entry 'Hulda' cannot be embedded")));

    // The failed open changed nothing: the run still holds the host data it had, under its hash.
    store::RunStore stored = store::RunStore::Open(file);
    CHECK(stored.HostData() == turn::HostDataToJson(original));
    CHECK(stored.Meta().worldHash == turn::HostDataHash(original));
}

TEST_CASE("Open refuses a file that holds an owned run", "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_owned");
    store::RunMeta meta;
    meta.worldId = "mini";
    {
        store::RunStore owned = store::RunStore::Create(file, meta, nlohmann::json::object());
    }
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend),
                         core::DataError,
                         MessageMatches(ContainsSubstring("is an owned run of world 'mini', not a hosted run")));
}

TEST_CASE("Open checks the host data before it touches the file, and leaves no file when the lore fails",
          "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_bad_data");
    turn::HostData data = test::FixtureHostData();
    data.catalog = R"({"actions": []})"_json;
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend),
                      "catalog at /actions: 'actions' must be a non-empty array");
    CHECK_FALSE(std::filesystem::exists(file));

    // Lore the embedding model cannot take is the host's DataError, and the new file goes again.
    test::ScriptedBackend small(16384);
    small.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, small),
                         core::DataError,
                         MessageMatches(ContainsSubstring("host data: lore entry 'Hulda' cannot be embedded")));
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("StartConversation stores the participants with their names and bios, in order", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_start"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::AELA, test::HULDA}, observation);
    CHECK(id == "c1");
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    REQUIRE(conversation->participants.size() == 2);
    CHECK(conversation->participants[0].id == test::AELA);
    CHECK(conversation->participants[0].name == "Aela");
    CHECK(conversation->participants[0].bio.empty());
    CHECK(conversation->participants[1].name == "Hulda");
    CHECK(conversation->participants[1].bio == "Hulda keeps the Bannered Mare.");
    CHECK_FALSE(conversation->ended);
    CHECK(session.StartConversation({test::SAADIA}, observation) == "c2");

    // Each fault is the host's DataError naming the field, and starts nothing.
    CHECK_THROWS_WITH(session.StartConversation({}, observation),
                      "participants: a conversation needs at least one participant");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, "00000000"}, observation),
                      "participants/1: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, test::HULDA}, observation),
                      "participants/1: '0001A6A4' is listed twice");
    nlohmann::json broken = observation;
    broken.at("actors").at(0)["id"] = "";
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA}, broken), ContainsSubstring("observation at /actors/0/id"));
    CHECK_FALSE(session.Conversation("c3"));
}

TEST_CASE("RecordEvents adds events to an open conversation, after its latest turn", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_events"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.RecordEvents(id, {R"({"type": "ambient", "text": "Rain starts."})"_json});
    session.RecordEvents(id, {R"({"text": "A door opens."})"_json, R"({"text": "Saadia looks up."})"_json});
    const std::vector<core::ConversationEvent> events = session.Events(id);
    REQUIRE(events.size() == 3);
    CHECK(events[0].afterTurn == 0); // Before the conversation's first turn.
    CHECK(events[0].event.at("type") == "ambient");
    CHECK(events[2].event.at("text") == "Saadia looks up.");

    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"type": "ambient"})"_json}),
                      "events/0/text: an event needs 'text', a string");
    CHECK_THROWS_WITH(session.RecordEvents("c9", {}), "conversation_id: there is no conversation 'c9'");
    CHECK(session.Events(id).size() == 3);
}

TEST_CASE("SetParticipants removes those who left, then adds those who joined at the end", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_participants"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::HULDA, test::AELA, test::SAADIA});
    CHECK(session.Conversation(id)->participants[2].bio == "Saadia serves at the inn.");
    // A leave and a rejoin in one call moves the actor to the end.
    session.SetParticipants(id, {test::HULDA}, {test::HULDA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::AELA, test::SAADIA, test::HULDA});
    session.SetParticipants(id, {}, {test::AELA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});

    // Each fault is the host's DataError naming the field, and changes nothing.
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, observation),
                      "joined/0: '0001B07C' is already a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {"00000000"}, {}, observation),
                      "joined/0: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::AELA}, observation),
                      "left/0: '00000007' is not a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::SAADIA, test::HULDA}, observation),
                      "left: conversation 'c1' would have no participant left");
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});
}

TEST_CASE("EndConversation ends a conversation once, and an ended conversation takes no more changes",
          "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_end"), test::FixtureHostData(),
                                                            test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(id);
    CHECK(session.Conversation(id)->ended);
    CHECK_THROWS_WITH(session.EndConversation(id), "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"text": "Late."})"_json}),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, test::FixtureObservation()),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.EndConversation("c9"), "conversation_id: there is no conversation 'c9'");
}
