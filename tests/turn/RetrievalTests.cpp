#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <string>
#include <vector>

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

} // namespace

TEST_CASE("Lore retrieval ranks shared words first and skips always-on lore", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_lore", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("Do storms arrive at dusk here?", ALL_SOURCES, 1000, 6, backend);
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

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("keeper", ALL_SOURCES, 1000, 6, backend);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].refId == "keeper");
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"keeper", "keeper: The keeper says \"hello\" to everyone."}, inference::EmbedPurpose::Document);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    CHECK(hits[0].score == Catch::Approx(similarity + turn::NAME_BOOST));

    const int oneSnippet = backend.CountTokens("- storm: Storms arrive at dusk.\n");
    CHECK(index.Retrieve("storm keeper dusk everyone", ALL_SOURCES, oneSnippet, 6, backend).size() == 1);
    CHECK(index.Retrieve("storm keeper dusk everyone", ALL_SOURCES, 1000, 1, backend).size() == 1);
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

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("brass key", ALL_SOURCES, 1000, 6, backend);
    bool foundTurn = false;
    bool foundNote = false;
    for (const turn::RetrievalHit& hit : hits) {
        CHECK(hit.refId != "2");
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
