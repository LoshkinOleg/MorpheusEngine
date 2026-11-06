#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"
#include "turn/Review.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("ParseReviewOutput reads a verdict, its problems and a digest", "[review]")
{
    const turn::Review consistent =
        turn::ParseReviewOutput(R"({"consistent":true,"problems":[],"digest":"  The player waited. "})", true, true);
    CHECK(consistent.error.empty());
    CHECK(consistent.consistent == true);
    CHECK(consistent.problems.empty());
    CHECK(consistent.digest == "The player waited."); // Trimmed.

    const turn::Review contradicts =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["The roll failed."]})", true, false);
    CHECK(contradicts.error.empty());
    CHECK(contradicts.consistent == false);
    CHECK(contradicts.problems == std::vector<std::string>{"The roll failed."});
    CHECK(contradicts.digest.empty());

    // With the check off only the digest is read: a verdict in the output anyway is ignored.
    const turn::Review digestOnly =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["x"],"digest":"The player waited."})", false, true);
    CHECK(digestOnly.error.empty());
    CHECK_FALSE(digestOnly.consistent);
    CHECK(digestOnly.problems.empty());
    CHECK(digestOnly.digest == "The player waited.");
}

TEST_CASE("ParseReviewOutput reports output it cannot read and keeps nothing of it", "[review]")
{
    // Each case: the output, the parts asked for, and a piece of the reason.
    struct Case {
        std::string text = "";
        bool check = false;
        bool digest = false;
        const char* reason = "";
    };
    const std::vector<Case> cases = {
        {R"({"consistent":false,"probl)", true, true, "not complete JSON"}, // Cut off by review_max_tokens.
        {R"(["consistent"])", true, true, "not a JSON object"},
        {R"({"problems":[],"digest":"x"})", true, true, "'consistent'"},
        {R"({"consistent":true,"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[1],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":false,"problems":[],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":["odd"],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[]})", true, true, "'digest'"},
        {R"({"digest":7})", false, true, "'digest'"},
        {R"({"consistent":false,"problems":["a","b","c","d"]})", true, false, "'problems'"}, // Over the bound.
        {R"({"consistent":false,"problems":[")" + std::string(201, 'p') + R"("]})", true, false, "'problems'"},
        {R"({"consistent":false,"problems":[""]})", true, false, "'problems'"}, // Empty is not a problem.
        {R"({"consistent":false,"problems":["  "]})", true, false, "'problems'"},
        {R"({"digest":")" + std::string(301, 'd') + R"("})", false, true, "'digest'"},
    };
    for (const Case& entry : cases) {
        INFO(entry.text);
        const turn::Review review = turn::ParseReviewOutput(entry.text, entry.check, entry.digest);
        CHECK_THAT(review.error, ContainsSubstring(entry.reason));
        CHECK_FALSE(review.consistent);
        CHECK(review.problems.empty());
        CHECK(review.digest.empty());
    }
    CHECK_THROWS_AS(turn::ParseReviewOutput("{}", false, false), core::ContractError);
}

TEST_CASE("ReviewPrompt shows the input, the whole outcome and the narration under the review rules", "[review]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks"},)"
        R"({"op":"add","path":"player.inventory.oil","value":-1,"reason":"spills"}]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const inference::Prompt prompt =
        turn::ReviewPrompt("I walk to the lighthouse.", proposal, resolution, "You walk to the lighthouse.");
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.stableCount == 1);
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == std::string(turn::REVIEW_RULES));
    CHECK(prompt.messages[1].role == inference::Role::User);
    CHECK(prompt.messages[1].content == "# Player\nI walk to the lighthouse.\n\n" +
                                            turn::RenderOutcome(proposal, resolution, {}) +
                                            "\n# Narration\nYou walk to the lighthouse.\n\n# Task\n" +
                                            std::string(turn::REVIEW_INSTRUCTION));
    // The outcome is shown in full, rejected changes and their reasons included.
    CHECK_THAT(prompt.messages[1].content,
               ContainsSubstring("Rejected: add player.inventory.oil (no item 'oil' to decrease)"));
}
