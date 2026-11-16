#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
#include "turn/PromptPack.h"
#include "turn/ResolveGrammar.h"
#include "turn/Review.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";
const std::string CONSISTENT = R"({"consistent":true,"problems":[]})";
const std::string PLAYER_TEXT = "Wait here for me.";
const std::vector<std::string> REVIEWED_TAGS = {"resolve", "speak", "review"};

// What a turn delivered: the pieces joined, and how many calls the backend had seen at the first piece.
struct Delivery {
    std::string text = {};
    std::size_t callsAtFirstPiece = 0;
    int pieces = 0;
};

// Callbacks that record what reaches onToken in `delivery`.
turn::HostedCallbacks Recording(Delivery& delivery, const test::ScriptedBackend& backend)
{
    turn::HostedCallbacks callbacks;
    callbacks.onToken = [&delivery, &backend](std::string_view piece) {
        if (delivery.pieces == 0) {
            delivery.callsAtFirstPiece = backend.Calls().size();
        }
        ++delivery.pieces;
        delivery.text += piece;
    };
    return callbacks;
}

// A run in a fresh file for test `name` with host data `data` and `settings`, on `backend`.
turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend,
                            turn::HostData data = test::FixtureHostData(),
                            core::Settings settings = test::HostedSettings())
{
    return turn::HostedSession::Open(test::HostedRunFile(name), std::move(data), std::move(settings), 42, backend);
}

// The tags of the backend's calls, in order.
std::vector<std::string> Tags(const test::ScriptedBackend& backend)
{
    std::vector<std::string> tags;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        tags.push_back(call.spec.callTag);
    }
    return tags;
}

} // namespace

TEST_CASE("Without review a turn is as before", "[hosted][review]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello there."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_review_off", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());

    Delivery delivery;
    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, test::HULDA, PLAYER_TEXT), Recording(delivery, backend), {});
    REQUIRE(record);
    CHECK(backend.Calls().size() == 2);
    CHECK(delivery.callsAtFirstPiece == 2); // The line streamed during the speak call.
    CHECK(delivery.pieces > 1);
    CHECK(delivery.text == "Hello there.");
    CHECK_FALSE(record->timings.contains("review"));
    CHECK_FALSE(record->timings.contains("respeak"));
}

TEST_CASE("A consistent review: the line is delivered once, after the review", "[hosted][review]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello there."};
    script.reviews = {CONSISTENT};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_review_consistent", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
    request.review = true;

    Delivery delivery;
    const std::optional<core::HostedTurn> record = session.RunTurn(request, Recording(delivery, backend), {});
    REQUIRE(record);
    CHECK(Tags(backend) == REVIEWED_TAGS);
    CHECK(delivery.pieces == 1);
    CHECK(delivery.callsAtFirstPiece == 3); // Held back until the review was made.
    CHECK(delivery.text == record->line);
    CHECK(record->line == "Hello there.");
    REQUIRE(record->timings.contains("review"));
    CHECK(record->timings.at("review").at("consistent") == true);
    CHECK(record->timings.at("review").at("problems").empty());
    CHECK_FALSE(record->timings.at("review").contains("error"));
    CHECK_FALSE(record->timings.contains("respeak"));

    // The review call: its grammar, its answer allowance and its standalone prompt.
    const test::ScriptedBackend::Call& call = backend.Calls().at(2);
    CHECK(call.spec.grammar == turn::BuildReviewGrammar(true, false));
    CHECK(call.spec.maxTokens == test::HostedSettings().budget.reviewMaxTokens);
    const inference::Prompt expected =
        turn::HostedReviewPrompt(std::string(turn::HOSTED_REVIEW_RULES), "Hulda", "Aela: " + PLAYER_TEXT,
                                 record->adjudication, test::FixtureObservation(), "Hello there.");
    REQUIRE(call.prompt.messages.size() == expected.messages.size());
    for (std::size_t i = 0; i < expected.messages.size(); ++i) {
        CHECK(call.prompt.messages[i].role == expected.messages[i].role);
        CHECK(call.prompt.messages[i].content == expected.messages[i].content);
    }
    CHECK(call.prompt.stableCount == expected.stableCount);
}

TEST_CASE("An inconsistent review: the line is spoken once more with the problems", "[hosted][review]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hulda [wait]: Fine.", "Fine, I will wait."};
    script.reviews = {R"({"consistent":false,"problems":["The line starts with a label."]})"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_review_respeak", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
    request.review = true;

    Delivery delivery;
    const std::optional<core::HostedTurn> record = session.RunTurn(request, Recording(delivery, backend), {});
    REQUIRE(record);
    CHECK(Tags(backend) == std::vector<std::string>{"resolve", "speak", "review", "respeak"});

    // The respeak prompt is the speak prompt with the correction and the problems before the speak message.
    const inference::Prompt& speak = backend.Calls().at(1).prompt;
    const inference::Prompt& respeak = backend.Calls().at(3).prompt;
    REQUIRE(respeak.messages.size() == speak.messages.size());
    for (std::size_t i = 0; i + 1 < speak.messages.size(); ++i) {
        CHECK(respeak.messages[i].content == speak.messages[i].content);
    }
    CHECK(respeak.messages.back().content == std::string(turn::HOSTED_RESPEAK_CORRECTION) +
                                                 "\n- The line starts with a label.\n\n" +
                                                 speak.messages.back().content);

    // Only the respoken line reaches the host, in one piece, and it is the committed one.
    CHECK(record->line == "Fine, I will wait.");
    CHECK(delivery.text == "Fine, I will wait.");
    CHECK(delivery.pieces == 1);
    CHECK(delivery.callsAtFirstPiece == 4);
    CHECK_THAT(delivery.text, !ContainsSubstring("Hulda [wait]"));
    CHECK(record->timings.at("review").at("consistent") == false);
    CHECK(record->timings.at("review").at("problems").size() == 1);
    REQUIRE(record->timings.contains("respeak"));
}

TEST_CASE("An unreadable review keeps the first line", "[hosted][review]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello there."};
    script.reviews = {"not json"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_review_unreadable", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
    request.review = true;

    Delivery delivery;
    const std::optional<core::HostedTurn> record = session.RunTurn(request, Recording(delivery, backend), {});
    REQUIRE(record);
    CHECK(Tags(backend) == REVIEWED_TAGS);
    CHECK(record->line == "Hello there.");
    CHECK(delivery.text == "Hello there.");
    REQUIRE(record->timings.contains("review"));
    CHECK_FALSE(record->timings.at("review").at("error").get<std::string>().empty());
    CHECK_FALSE(record->timings.at("review").contains("consistent"));
    CHECK_FALSE(record->timings.contains("respeak"));
}

TEST_CASE("A respeak message over narrate_allowance drops problems", "[hosted][review]")
{
    const std::string longOne = "Hulda talked at length about the cold weather outside the inn this morning.";
    const std::string longTwo = "Hulda also named the price of every tankard on the shelf behind the bar.";
    const std::string review = nlohmann::json{{"consistent", false}, {"problems", {longOne, longTwo}}}.dump();

    // The speak instruction as the run sends it, and the bare correction message (no problem) around it.
    const turn::HostedWorld world(test::FixtureHostData());
    const std::string speak = turn::PromptPack::Fill(world.Prompts().speak, {.speaker = "Hulda", .player = "Aela"});
    const std::string bare = std::string(turn::HOSTED_RESPEAK_CORRECTION) + "\n\n" + speak;
    // The fake counts (bytes + 3) / 4 per text.
    auto tokens = [](const std::string& text) { return (static_cast<int>(text.size()) + 3) / 4; };
    REQUIRE(tokens(bare) > tokens(speak) + 1);

    // The respeak call's last message when narrate_allowance is `allowance`.
    auto respeakMessage = [&](int allowance) {
        test::HostedScript script;
        script.actions = {NONE};
        script.lines = {"Wrong.", "Right."};
        script.reviews = {review};
        test::ScriptedBackend backend(16384, script.Responder());
        core::Settings settings = test::HostedSettings();
        settings.budget.narrateAllowance = allowance;
        turn::HostedSession session = OpenRun("hosted_review_allowance_" + std::to_string(allowance), backend,
                                              test::FixtureHostData(), settings);
        const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
        request.review = true;
        REQUIRE(session.RunTurn(request, {}, {}));
        REQUIRE(backend.Calls().size() == 4);
        // The speak message is the pack's, whatever the allowance.
        CHECK(backend.Calls().at(1).prompt.messages.back().content == speak);
        return backend.Calls().at(3).prompt.messages.back().content;
    };

    // Room for the bare correction and the speak instruction, not for a problem: both problems are dropped.
    const std::string fitting = respeakMessage(tokens(bare) + 1);
    CHECK(fitting == bare);
    CHECK_THAT(fitting, !ContainsSubstring(longOne));
    CHECK_THAT(fitting, !ContainsSubstring(longTwo));

    // Room for the speak instruction alone: the plain speak message.
    const std::string plain = respeakMessage(tokens(speak));
    CHECK(plain == speak);
}

TEST_CASE("A host pack's review text is the review call's system message", "[hosted][review]")
{
    turn::HostData data = test::FixtureHostData();
    data.prompts = {{"review", "MY RULES"}};
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello there."};
    script.reviews = {CONSISTENT};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_review_pack", backend, data);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
    request.review = true;

    REQUIRE(session.RunTurn(request, {}, {}));
    REQUIRE(backend.Calls().size() == 3);
    const inference::Prompt& prompt = backend.Calls().at(2).prompt;
    CHECK(prompt.messages.front().role == inference::Role::System);
    CHECK(prompt.messages.front().content == "MY RULES");
}

TEST_CASE("A stop during the review commits nothing", "[hosted][review]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Hello there."};
    script.reviews = {CONSISTENT};
    std::stop_source stopSource;
    test::ScriptedBackend::Responder scripted = script.Responder();
    // The stop is requested as the review call starts, so its reply is cancelled at its first piece.
    test::ScriptedBackend backend(
        16384, [&](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            if (spec.callTag == "review") {
                stopSource.request_stop();
            }
            return scripted(prompt, spec);
        });
    turn::HostedSession session = OpenRun("hosted_review_stop", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, PLAYER_TEXT);
    request.review = true;

    Delivery delivery;
    CHECK_FALSE(session.RunTurn(request, Recording(delivery, backend), stopSource.get_token()));
    CHECK(Tags(backend) == REVIEWED_TAGS);
    CHECK(session.Turns(id).empty());
    CHECK(delivery.pieces == 0);
    CHECK(delivery.text.empty());
}
