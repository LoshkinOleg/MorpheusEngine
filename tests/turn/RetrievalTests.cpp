#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr std::array<store::EmbeddingKind, 3> ALL_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                             store::EmbeddingKind::Note};

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

store::RunStore NewRun(const std::string& name, const core::World& world)
{
    store::RunMeta meta;
    meta.worldId = world.id;
    meta.worldHash = world.contentHash;
    meta.modelId = "test-chat";
    meta.rngSeed = 1;
    return store::RunStore::Create(test::FreshTempDir(name) / "run.db", meta, world.initialState);
}

// The cosine of two texts under the scripted embedding, as Retrieve computes it.
float Similarity(test::ScriptedBackend& backend, const std::string& a, const std::string& b)
{
    const std::vector<std::vector<float>> vectors = backend.Embed({a, b}, inference::EmbedPurpose::Query);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    return similarity;
}

// The hit for lore `subject`, or nullptr.
const turn::RetrievalHit* FindLore(const std::vector<turn::RetrievalHit>& hits, const std::string& subject)
{
    for (const turn::RetrievalHit& hit : hits) {
        if (hit.kind == store::EmbeddingKind::Lore && hit.refId == subject) {
            return &hit;
        }
    }
    return nullptr;
}

// The sandcrawler world: its scene is the player's and the crawler's location, present at the player's.
core::World Sandcrawler()
{
    return core::LoadWorld(test::RepoDir() / "game_projects" / "sandcrawler");
}

// A sandcrawler NPC with `role`, at `location`.
nlohmann::json Npc(const std::string& role, const std::string& location)
{
    return {{"type", "npc"},
            {"fields",
             {{"role", role}, {"disposition", "wary"}, {"location", location},
              {"conditions", nlohmann::json::array()}, {"health", 10}}}};
}

} // namespace

TEST_CASE("Lore retrieval ranks shared words first and skips always-on lore", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_lore", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);

    const std::vector<turn::RetrievalHit> hits = index.Retrieve(turn::RetrievalQuery{"Do storms arrive at dusk here?"}, ALL_SOURCES, 1000, 6, backend);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].kind == store::EmbeddingKind::Lore);
    CHECK(hits[0].refId == "storm");
    CHECK(hits[0].text == "storm: Storms arrive at dusk.");
    for (const turn::RetrievalHit& hit : hits) {
        CHECK(hit.refId != "lighthouse"); // Always-on lore is in L1, never retrieved.
    }
}

TEST_CASE("Stored lore vectors are reused until the text or the prefix changes", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_reuse", world);
    test::ScriptedBackend backend(16384);

    turn::MemoryIndex(std::string("search_document: ")).SyncLore(world, store, backend);
    CHECK(backend.EmbeddedTexts() == 2); // keeper and storm.
    turn::MemoryIndex(std::string("search_document: ")).SyncLore(world, store, backend);
    CHECK(backend.EmbeddedTexts() == 2); // Reused from the store.

    core::World edited = world;
    edited.lore[2].data = "Storms arrive at dawn."; // lore.csv order: lighthouse, keeper, storm.
    turn::MemoryIndex(std::string("search_document: ")).SyncLore(edited, store, backend);
    CHECK(backend.EmbeddedTexts() == 3);
    turn::MemoryIndex(std::string("passage: ")).SyncLore(edited, store, backend);
    CHECK(backend.EmbeddedTexts() == 5);
}

TEST_CASE("A lore subject named in the input is boosted, and hits stay within the cap", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_boost", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);

    const std::vector<turn::RetrievalHit> hits = index.Retrieve(turn::RetrievalQuery{"keeper"}, ALL_SOURCES, 1000, 6, backend);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].refId == "keeper");
    CHECK(hits[0].reasons == std::vector<std::string>{"name"});
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"keeper", "keeper: The keeper says \"hello\" to everyone."}, inference::EmbedPurpose::Document);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    CHECK(hits[0].score == Catch::Approx(similarity + turn::NAME_BOOST));

    const int oneSnippet = backend.CountTokens("- storm: Storms arrive at dusk.\n");
    CHECK(index.Retrieve(turn::RetrievalQuery{"storm keeper dusk everyone"}, ALL_SOURCES, oneSnippet, 6, backend).size() == 1);
    CHECK(index.Retrieve(turn::RetrievalQuery{"storm keeper dusk everyone"}, ALL_SOURCES, 1000, 1, backend).size() == 1);
}

TEST_CASE("Only folded turns and notes are searchable history", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_history", world);
    core::TurnRecord first;
    first.number = 1;
    first.input = "I find a brass key.";
    first.narration = "The brass key is cold.";
    first.notes = {"The brass key opens the cellar."};
    store.CommitTurn(first, world.initialState);
    core::TurnRecord second;
    second.number = 2;
    second.input = "I find another brass key.";
    second.narration = "Two brass keys now.";
    store.CommitTurn(second, world.initialState);

    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncHistory(store, 1, backend); // Turn 2 is still in the transcript.

    const std::vector<turn::RetrievalHit> hits = index.Retrieve(turn::RetrievalQuery{"brass key"}, ALL_SOURCES, 1000, 6, backend);
    bool foundTurn = false;
    bool foundNote = false;
    for (const turn::RetrievalHit& hit : hits) {
        CHECK(hit.refId != "2");
        CHECK(hit.reasons.empty()); // History and notes have no subject: similarity alone.
        if (hit.kind == store::EmbeddingKind::Turn && hit.refId == "1") {
            foundTurn = true;
            CHECK(hit.text == "Turn 1: I find a brass key. -> The brass key is cold.");
        }
        if (hit.kind == store::EmbeddingKind::Note && hit.refId == "1.0") {
            foundNote = true;
            CHECK(hit.text == "Note from turn 1: The brass key opens the cellar.");
        }
    }
    CHECK(foundTurn);
    CHECK(foundNote);
}

TEST_CASE("Lore that cannot be embedded or repeats a subject is a world error", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_errors", world);
    test::ScriptedBackend backend(16384);
    backend.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::MemoryIndex(std::string("p: ")).SyncLore(world, store, backend), core::DataError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("keeper")));

    test::ScriptedBackend roomy(16384);
    core::World doubled = world;
    doubled.lore.push_back({"storm", "Another storm.", false});
    CHECK_THROWS_MATCHES(turn::MemoryIndex(std::string("p: ")).SyncLore(doubled, store, roomy), core::DataError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("two entries")));
}

TEST_CASE("A history text that cannot be embedded is a runtime failure, not a world error", "[retrieval]")
{
    // History and note texts are cut by the engine, so a failure to embed one is the backend's (InferenceError),
    // never bad world or memory data.
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_history_error", world);
    core::TurnRecord first;
    first.number = 1;
    first.input = "I find a brass key.";
    first.narration = "The brass key is cold.";
    store.CommitTurn(first, world.initialState);

    test::ScriptedBackend backend(16384);
    backend.SetEmbedLimit(20);
    CHECK_THROWS_AS(turn::MemoryIndex(std::string("p: ")).SyncHistory(store, 1, backend), inference::InferenceError);
}

TEST_CASE("RetrievalQuery joins the input, the scene and the digest", "[retrieval]")
{
    CHECK(turn::RetrievalQuery{"I walk.", {"shore", "keeper"}, "The player rested."}.Text() ==
          "I walk.\nshore; keeper\nThe player rested.");
    CHECK(turn::RetrievalQuery{"I walk.", {}, ""}.Text() == "I walk.");
    CHECK(turn::RetrievalQuery{"I walk.", {}, "The player rested."}.Text() == "I walk.\nThe player rested.");

    // The input comes first, so it survives the cut to EMBED_TEXT_CHARS.
    const std::string longInput(turn::EMBED_TEXT_CHARS + 10, 'a');
    CHECK(turn::RetrievalQuery{longInput, {"shore"}, "x"}.Text() == longInput.substr(0, turn::EMBED_TEXT_CHARS));
}

TEST_CASE("A lore subject named by the scene is boosted once, with its reasons", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_scene", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);
    const std::string keeper = "keeper: The keeper says \"hello\" to everyone.";

    SECTION("named by the scene only")
    {
        const turn::RetrievalQuery query{"I look around.", {"near the keeper's door"}, ""};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons == std::vector<std::string>{"scene"});
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper) + turn::NAME_BOOST));
    }
    SECTION("named by the input and the scene: one boost, both reasons")
    {
        const turn::RetrievalQuery query{"I greet the keeper.", {"the keeper"}, ""};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons == std::vector<std::string>{"name", "scene"});
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper) + turn::NAME_BOOST));
    }
    SECTION("the digest only enters the embedding")
    {
        const turn::RetrievalQuery query{"I look around.", {}, "The keeper waved."};
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, 1000, 6, backend);
        const turn::RetrievalHit* pHit = FindLore(hits, "keeper");
        REQUIRE(pHit != nullptr);
        CHECK(pHit->reasons.empty());
        CHECK(pHit->score == Catch::Approx(Similarity(backend, query.Text(), keeper)));
    }
    SECTION("scene hits are still trimmed to the cap")
    {
        const turn::RetrievalQuery query{"I look around.", {"keeper", "storm"}, ""};
        const int oneSnippet = backend.CountTokens("- storm: Storms arrive at dusk.\n");
        const std::vector<turn::RetrievalHit> hits = index.Retrieve(query, ALL_SOURCES, oneSnippet, 6, backend);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].refId == "storm"); // The keeper's longer snippet does not fit.
    }
}

TEST_CASE("SceneOf lists the scene's values and who is present", "[retrieval]")
{
    const core::World world = Sandcrawler();
    nlohmann::json state = world.initialState; // The captain and the crawler are both in the "open desert".

    SECTION("the declared paths, then the entities at the player's location")
    {
        // The crawler shares the captain's location; it has no other string field.
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert", "crawler"});
    }
    SECTION("an NPC at the player's location adds its id and role, compared without case or spaces")
    {
        state["npc:marta"] = Npc("mechanic", "  Open Desert ");
        state["npc:raider"] = Npc("scout", "the ridge"); // Elsewhere: adds nothing.
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert", "crawler", "npc:marta", "mechanic"});
    }
    SECTION("duplicates collapse, keeping the first")
    {
        state["npc:marta"] = Npc("mechanic", "open desert");
        state["npc:otto"] = Npc("Mechanic", "open desert");
        CHECK(turn::SceneOf(world, state) ==
              std::vector<std::string>{"open desert", "crawler", "npc:marta", "mechanic", "npc:otto"});
    }
    SECTION("empty values are skipped")
    {
        state["crawler"]["fields"]["location"] = "";
        CHECK(turn::SceneOf(world, state) == std::vector<std::string>{"open desert"});
    }
    SECTION("a world without a scene has none")
    {
        CHECK(turn::SceneOf(MiniWorld(), MiniWorld().initialState).empty());
    }
}
