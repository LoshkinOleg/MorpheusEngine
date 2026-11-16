#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "turn/Session.h"

using namespace morpheus;
namespace fs = std::filesystem;

namespace {

const std::string MOVE = R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks there"}]})";
const std::string LOOKUPS = R"({"lookups":[{"source":"lore","query":"keeper"}]})";
const std::string CONTRADICTS = R"({"consistent":false,"problems":["The narration has the player fly."],"digest":"d"})";
const std::string CONSISTENT = R"({"consistent":true,"problems":[],"digest":"d"})";

// The tags the turn layer may put on a call of the owned game.
const std::set<std::string> OWNED_TAGS = {"lookup", "resolve", "narrate", "renarrate", "review", "summarize", "condense"};

// A scripted model that answers by the call's tag and records every tag in order. Reviews answer `reviews` in
// order. The narration is `narration`. The recorder must outlive the backend that calls it.
struct TagScript {
    std::string narration = "You walk on.";
    std::vector<std::string> reviews = {};
    std::vector<std::string> tags = {};
    std::size_t reviewed = 0;

    test::ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt&, const inference::GenerationSpec& spec) -> std::string {
            tags.push_back(spec.callTag);
            if (spec.callTag == "lookup") {
                return LOOKUPS;
            }
            if (spec.callTag == "resolve") {
                return MOVE;
            }
            if (spec.callTag == "review") {
                MORPHEUS_REQUIRE(reviewed < reviews.size(), "TagScript: no review reply left");
                return reviews[reviewed++];
            }
            if (spec.callTag == "summarize" || spec.callTag == "condense") {
                return "Summary.";
            }
            return narration; // narrate and renarrate
        };
    }

    bool Has(const std::string& tag) const
    {
        for (const std::string& seen : tags) {
            if (seen == tag) {
                return true;
            }
        }
        return false;
    }

    // Every recorded tag is one of the owned tags.
    bool AllOwned() const
    {
        for (const std::string& seen : tags) {
            if (!OWNED_TAGS.contains(seen)) {
                return false;
            }
        }
        return true;
    }
};

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

core::Settings TestSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {256, 256, 256, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// For a context of 3000 with 2000-byte narrations: two turns fit the transcript, three do not (SessionTests).
core::Settings FoldingSettings()
{
    core::Settings settings = TestSettings();
    settings.budget = {64, 600, 200, 400, 1024, 1024, 512, 100};
    return settings;
}

fs::path NewRunFile(const std::string& name)
{
    return test::FreshTempDir(name) / "run.db";
}

} // namespace

TEST_CASE("A turn with lookups, a review and a renarrate tags its calls", "[calltag]")
{
    // Assisted memory makes the lookup call; the first review finds a contradiction, so the narration is redone.
    core::Settings settings = TestSettings();
    settings.memoryMode = core::MemoryMode::Assisted;
    settings.narrationCheck = true;
    settings.budget.reviewMaxTokens = 384;
    TagScript script;
    script.reviews = {CONTRADICTS, CONSISTENT};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), settings, NewRunFile("calltag_turn"), 42, backend);
    REQUIRE(session.RunTurn("I walk.", {}, {}));

    CHECK(script.tags == std::vector<std::string>{"lookup", "resolve", "narrate", "review", "renarrate", "review"});
    CHECK(script.AllOwned());
}

TEST_CASE("A fold tags its call summarize", "[calltag]")
{
    TagScript script;
    script.narration = std::string(2000, 'w');
    test::ScriptedBackend backend(3000, script.Responder());
    turn::Session session =
        turn::Session::Start(MiniWorld(), FoldingSettings(), NewRunFile("calltag_fold"), 42, backend);
    // No Maintain between turns: from the fourth turn on, the turn's own fit loop folds.
    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn("I walk.", {}, {}));
    }
    CHECK(script.Has("summarize"));
    CHECK_FALSE(script.Has("condense"));
    CHECK(script.AllOwned());
}

TEST_CASE("A condense tags its call condense", "[calltag]")
{
    const fs::path file = NewRunFile("calltag_condense");
    TagScript script;
    script.narration = std::string(2000, 'w');
    test::ScriptedBackend backend(3000, script.Responder());
    {
        // Each Maintain from the third turn on folds one turn into a summary of its own.
        turn::Session session = turn::Session::Start(MiniWorld(), FoldingSettings(), file, 42, backend);
        for (int i = 0; i < 6; ++i) {
            REQUIRE(session.RunTurn("I walk.", {}, {}));
            REQUIRE(session.Maintain({}));
        }
        REQUIRE(session.Summaries().size() >= 3);
    }
    CHECK_FALSE(script.Has("condense"));

    // Three or more summaries are over a cap of 16 tokens; the next turn condenses them.
    core::Settings smallCap = FoldingSettings();
    smallCap.budget.summaryCap = 16;
    turn::Session session = turn::Session::Load(MiniWorld(), smallCap, file, backend);
    REQUIRE(session.RunTurn("I walk.", {}, {}));
    CHECK(script.Has("condense"));
    CHECK(script.AllOwned());
}
