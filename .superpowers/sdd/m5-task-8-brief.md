### Task 8: Scene retrieval

Automatic retrieval searches with the player's input, the scene values and the previous turn's digest, joined by newlines and cut to `EMBED_TEXT_CHARS` with the input first. A lore subject named in the input or in a scene value gets `NAME_BOOST` once, and each hit says why it matched. Assisted-mode lookups query with their own text only, as today.

**Files:**
- Create: `tests/SceneWorld.h`
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`, `src/turn/Session.cpp`
- Test: `tests/turn/RetrievalTests.cpp`, `tests/turn/SessionTests.cpp`

**Interfaces:**
- Consumes: `core::World::scene`, `core::SceneDecl` (Task 7); `core::TurnRecord::digest` (Task 2); `ReviewScript`, `ReviewSettings` (Task 4).
- Produces:
  - `std::vector<std::string> turn::SceneOf(const core::World& world, const nlohmann::json& state)`.
  - `struct turn::RetrievalQuery { std::string input; std::vector<std::string> scene; std::string digest; std::string Text() const; }`.
  - `std::vector<RetrievalHit> turn::MemoryIndex::Retrieve(const RetrievalQuery& query, std::span<const store::EmbeddingKind> sources, int capTokens, std::size_t maxHits, inference::LlmBackend& backend) const`.
  - `std::vector<std::string> turn::RetrievalHit::reasons` (after `tokens`): `"name"`, `"scene"`, or empty.
  - Telemetry: each `timings["retrieval"]` entry gains `"reasons"`; `timings["scene"]` holds the scene values (an empty array for a world without a scene).
  - `std::filesystem::path test::SceneWorldDir(std::string_view name)` (`tests/SceneWorld.h`): a mini world copy at `FreshTempDir(name)/mini` with the scene `player.location` and lore for `shore` and `cliff top`.

- [ ] **Step 1: Add the scene test world**

Create `tests/SceneWorld.h`:

```cpp
#pragma once

#include <filesystem>
#include <string_view>

#include "TestPaths.h"

namespace morpheus::test {

// A copy of the mini test world in FreshTempDir(name)/mini (the folder name is the world id) that declares a scene,
// the player's location, and has lore for two places: "shore", where the player starts, and "cliff top". The rest of
// the lore is the mini world's. Returns the world folder.
inline std::filesystem::path SceneWorldDir(std::string_view name)
{
    const std::filesystem::path dir = FreshTempDir(name) / "mini";
    std::filesystem::copy(TestDataDir() / "worlds" / "mini", dir, std::filesystem::copy_options::recursive);
    WriteFile(dir / "world.json",
              R"({"id": "mini", "title": "Mini World", "dice": "1d20+stat", "check_entity": "player",)"
              R"( "stats": ["agility"], "difficulties": {"easy": 8, "hard": 16}, "memory_mode": "engine",)"
              R"( "scene": {"paths": ["player.location"], "present_at": "player.location"}})");
    WriteFile(dir / "lore.csv", "subject,data,always_on\n"
                                "lighthouse,\"A tall lighthouse, painted red.\",true\n"
                                "keeper,\"The keeper says \"\"hello\"\" to everyone.\",false\n"
                                "storm,Storms arrive at dusk.,\n"
                                "shore,Grey pebbles and driftwood line the shore.,\n"
                                "cliff top,Wind howls over the bare cliff top.,\n");
    return dir;
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing retrieval tests**

In `tests/turn/RetrievalTests.cpp`, add to the includes:

```cpp
#include <cstddef>

#include <nlohmann/json.hpp>
```

Every existing `index.Retrieve("...", ...)` call takes a `turn::RetrievalQuery` now:

```bash
sed -i 's/index\.Retrieve("\([^"]*\)", /index.Retrieve(turn::RetrievalQuery{"\1"}, /' tests/turn/RetrievalTests.cpp
grep -c 'Retrieve(turn::RetrievalQuery{' tests/turn/RetrievalTests.cpp
```

The count is 5.

In "A lore subject named in the input is boosted, and hits stay within the cap", after `CHECK(hits[0].refId == "keeper");` add:

```cpp
    CHECK(hits[0].reasons == std::vector<std::string>{"name"});
```

In "Only folded turns and notes are searchable history", inside the loop, after `CHECK(hit.refId != "2");` add:

```cpp
        CHECK(hit.reasons.empty()); // History and notes have no subject: similarity alone.
```

Inside the anonymous namespace, after `NewRun`, add:

```cpp
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
```

Append:

```cpp
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
```

- [ ] **Step 3: Write the failing session tests**

In `tests/turn/SessionTests.cpp`, add to the includes:

```cpp
#include <catch2/catch_approx.hpp>

#include "SceneWorld.h"
```

Append:

```cpp
TEST_CASE("Retrieval searches with the scene: moving the player changes the lore in the prompt", "[session][scene]")
{
    const std::string climb =
        R"({"intent":"climb","checks":[],"changes":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs"}]})";
    test::ScriptedBackend backend(16384, Responder(climb, "You look around."));
    turn::Session session = turn::Session::Start(core::LoadWorld(test::SceneWorldDir("session_scene_world")),
                                                 TestSettings(), NewRunFile("session_scene"), 42, backend);
    // A lore hit's reasons in a turn's telemetry; null when it did not reach the prompt.
    auto reasonsOf = [](const core::TurnRecord& record, const std::string& subject) {
        for (const nlohmann::json& hit : record.timings.at("retrieval")) {
            if (hit.at("source") == "lore" && hit.at("ref") == subject) {
                return hit.at("reasons");
            }
        }
        return nlohmann::json();
    };

    // On the shore the input names nothing, but the scene names the shore.
    const std::optional<core::TurnRecord> first = session.RunTurn("I look around.", {}, {});
    REQUIRE(first);
    CHECK(first->timings.at("scene") == nlohmann::json::array({"shore"}));
    CHECK(reasonsOf(*first, "shore") == nlohmann::json::array({"scene"}));
    CHECK_THAT(Tail(backend.Calls().at(0).prompt), ContainsSubstring("- shore: Grey pebbles"));

    // The first turn moved the player to the cliff top: the same input now brings the cliff top's lore.
    const std::optional<core::TurnRecord> second = session.RunTurn("I look around.", {}, {});
    REQUIRE(second);
    CHECK(second->timings.at("scene") == nlohmann::json::array({"cliff top"}));
    CHECK(reasonsOf(*second, "cliff top") == nlohmann::json::array({"scene"}));
    const nlohmann::json shore = reasonsOf(*second, "shore");
    CHECK((shore.is_null() || shore.empty())); // No longer named by the scene.
    CHECK_THAT(Tail(backend.Calls().at(2).prompt), ContainsSubstring("- cliff top: Wind howls"));
}

TEST_CASE("Retrieval searches with the previous turn's digest, without a boost", "[session][scene]")
{
    ReviewScript script{MOVE, {"You wait.", "You wait some more."},
                        {R"({"digest":"Storms arrive at dusk."})", R"({"digest":"The player waited."})"}};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::Session session = turn::Session::Start(MiniWorld(), ReviewSettings(false, true),
                                                 NewRunFile("session_digest_query"), 42, backend);
    REQUIRE(session.RunTurn("I wait.", {}, {}));
    const std::optional<core::TurnRecord> second = session.RunTurn("I wait.", {}, {});
    REQUIRE(second);

    // The second query is the input, then the first turn's digest (the mini world declares no scene). The digest
    // names the storm, but only the input and the scene boost.
    const std::vector<std::vector<float>> vectors =
        backend.Embed({"I wait.\nStorms arrive at dusk.", "storm: Storms arrive at dusk."}, inference::EmbedPurpose::Query);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    const nlohmann::json* pStorm = nullptr;
    for (const nlohmann::json& hit : second->timings.at("retrieval")) {
        if (hit.at("ref") == "storm") {
            pStorm = &hit;
        }
    }
    REQUIRE(pStorm != nullptr);
    CHECK(pStorm->at("score").get<double>() == Catch::Approx(similarity));
    CHECK(pStorm->at("reasons").empty());
    CHECK(second->timings.at("scene") == nlohmann::json::array());
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `RetrievalQuery` and `SceneOf` are not members of `morpheus::turn`, and `reasons` is not a member of `RetrievalHit`.

- [ ] **Step 5: Add the query, the reasons and SceneOf**

In `src/turn/Retrieval.h`, add `#include <nlohmann/json.hpp>` to the includes. Replace the `NAME_BOOST` line with:

```cpp
constexpr float NAME_BOOST = 0.15f;                // Added once when a lore subject appears in the input or the scene.
```

Replace `RetrievalHit` with:

```cpp
// One retrieved snippet for the tail (T2).
struct RetrievalHit {
    store::EmbeddingKind kind = store::EmbeddingKind::Lore;
    std::string refId = {};
    std::string text = {}; // As the prompt shows it.
    float score = 0.0f;
    int tokens = 0; // What its prompt line costs against the retrieval cap (set by Retrieve).
    // Why it matched beyond similarity: "name" (its lore subject is in the input), "scene" (in a scene value). Empty
    // for similarity alone.
    std::vector<std::string> reasons = {};
};

// What retrieval searches with, in parts, so Retrieve can tell why an entry matched.
struct RetrievalQuery {
    std::string input = {};              // The player's input, or an assisted-mode lookup's query.
    std::vector<std::string> scene = {}; // SceneOf's values.
    std::string digest = {};             // The previous turn's digest.

    // What is embedded: the input, the scene values joined by "; ", then the digest, one per line (empty parts left
    // out), cut to EMBED_TEXT_CHARS bytes. The input comes first, so it survives the cut.
    std::string Text() const;
};

// The scene text for `state` (architecture spec, "World data"), empty for a world without a scene block: the value
// at each declared path, then every entity other than present_at's whose field of present_at's field name is a
// string equal to present_at's value (trimmed, lower-cased), in id order: its id, then its other string fields' values
// (in field name order). Empty values are skipped; values are deduplicated case-insensitively (trimmed), keeping the
// first as written.
std::vector<std::string> SceneOf(const core::World& world, const nlohmann::json& state);
```

Replace the `Retrieve` comment and declaration with:

```cpp
    // The best hits for `query` among `sources`. Each hit scores at least RETRIEVAL_MIN_SCORE: the cosine with the
    // embedding of query.Text(), plus NAME_BOOST, once, for a lore subject (lower-cased) contained in the input
    // (reason "name") or in a scene value (reason "scene"). The digest only enters the embedding. Hits come in score
    // order (ties by kind, then refId), at most maxHits of them, whose "- text\n" lines fit capTokens together. A hit
    // too big for the rest of the cap is skipped. No embedding call is made when nothing is searchable.
    std::vector<RetrievalHit> Retrieve(const RetrievalQuery& query, std::span<const store::EmbeddingKind> sources,
                                       int capTokens, std::size_t maxHits, inference::LlmBackend& backend) const;
```

In `src/turn/Retrieval.cpp`, after the anonymous namespace, add:

```cpp
std::string RetrievalQuery::Text() const
{
    // One part per line, in this order; empty parts are left out.
    std::string sceneText;
    for (const std::string& value : scene) {
        sceneText += (sceneText.empty() ? "" : "; ") + value;
    }
    const std::string* parts[] = {&input, &sceneText, &digest};
    std::string text;
    for (const std::string* pPart : parts) {
        if (!pPart->empty()) {
            text += (text.empty() ? "" : "\n") + *pPart;
        }
    }
    return core::CutUtf8(text, EMBED_TEXT_CHARS);
}

std::vector<std::string> SceneOf(const core::World& world, const nlohmann::json& state)
{
    if (!world.scene) {
        return {};
    }
    std::vector<std::string> values;
    std::set<std::string> seen; // Trimmed, lower-cased values kept so far.
    auto add = [&](const std::string& value) {
        const std::string key = Lowered(core::Trim(value));
        if (!key.empty() && seen.insert(key).second) {
            values.push_back(value);
        }
    };
    // A declared path's entity and field: "player.location" -> "player", "location". World loading checked the shape.
    auto split = [](const std::string& path) {
        const std::size_t dot = path.find('.');
        return std::pair<std::string, std::string>(path.substr(0, dot), path.substr(dot + 1));
    };
    // The string at `field` of an entity's fields, or "" when it has none.
    auto stringField = [](const nlohmann::json& entity, const std::string& field) {
        const nlohmann::json& fields = entity.at("fields");
        return fields.contains(field) && fields.at(field).is_string() ? fields.at(field).get<std::string>()
                                                                      : std::string();
    };

    // 1. The value at each declared path. Entities are never removed, so a scene entity is always in the state.
    for (const std::string& path : world.scene->paths) {
        const auto [entity, field] = split(path);
        MORPHEUS_REQUIRE(state.contains(entity), "SceneOf: a scene entity is missing from the state");
        add(stringField(state.at(entity), field));
    }

    // 2. Every other entity at present_at's value, by id (JSON objects iterate in key order): its id and its other
    // string fields. Nobody is present at an empty location.
    const auto [anchorEntity, anchorField] = split(world.scene->presentAt);
    MORPHEUS_REQUIRE(state.contains(anchorEntity), "SceneOf: the present_at entity is missing from the state");
    const std::string anchor = Lowered(core::Trim(stringField(state.at(anchorEntity), anchorField)));
    if (anchor.empty()) {
        return values;
    }
    for (auto it = state.begin(); it != state.end(); ++it) {
        if (it.key() == anchorEntity || Lowered(core::Trim(stringField(it.value(), anchorField))) != anchor) {
            continue;
        }
        add(it.key());
        const core::EntityTypeDef* pType = world.schema.FindEntityType(it.value().at("type").get<std::string>());
        MORPHEUS_REQUIRE(pType != nullptr, "SceneOf: the state has an unknown entity type");
        for (const auto& [name, def] : pType->fields) {
            if (def.type == core::FieldType::String && name != anchorField) {
                add(stringField(it.value(), name));
            }
        }
    }
    return values;
}
```

Replace `MemoryIndex::Retrieve` with:

```cpp
std::vector<RetrievalHit> MemoryIndex::Retrieve(const RetrievalQuery& query,
                                                std::span<const store::EmbeddingKind> sources, int capTokens,
                                                std::size_t maxHits, inference::LlmBackend& backend) const
{
    auto searched = [&sources](store::EmbeddingKind kind) {
        return std::find(sources.begin(), sources.end(), kind) != sources.end();
    };
    const std::string text = query.Text();
    const bool anything = std::any_of(_entries.begin(), _entries.end(), [&](const Entry& e) { return searched(e.kind); });
    if (!anything || capTokens <= 0 || maxHits == 0 || text.empty()) {
        return {};
    }

    // The whole query is embedded; the input and the scene values are also matched against lore subjects.
    const std::vector<float> queryVector = backend.Embed({text}, inference::EmbedPurpose::Query).at(0);
    const std::string loweredInput = Lowered(query.input);
    std::vector<std::string> loweredScene;
    for (const std::string& value : query.scene) {
        loweredScene.push_back(Lowered(value));
    }
    std::vector<RetrievalHit> hits;
    for (const Entry& entry : _entries) {
        if (!searched(entry.kind)) {
            continue;
        }
        MORPHEUS_REQUIRE(entry.vector.size() == queryVector.size(), "Retrieve: vectors from different models");
        float score = 0.0f;
        for (std::size_t i = 0; i < queryVector.size(); ++i) {
            score += queryVector[i] * entry.vector[i]; // Both are L2-normalized: the dot product is the cosine.
        }
        // A lore subject named in the input, in a scene value or in both is boosted once.
        std::vector<std::string> reasons;
        if (!entry.name.empty()) {
            if (loweredInput.find(entry.name) != std::string::npos) {
                reasons.push_back("name");
            }
            const bool inScene = std::any_of(loweredScene.begin(), loweredScene.end(), [&entry](const std::string& value) {
                return value.find(entry.name) != std::string::npos;
            });
            if (inScene) {
                reasons.push_back("scene");
            }
        }
        if (!reasons.empty()) {
            score += NAME_BOOST;
        }
        if (score >= RETRIEVAL_MIN_SCORE) {
            hits.push_back({entry.kind, entry.refId, entry.text, score, 0, std::move(reasons)});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RetrievalHit& a, const RetrievalHit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.refId < b.refId;
    });

    std::vector<RetrievalHit> kept;
    int used = 0;
    for (RetrievalHit& hit : hits) {
        if (kept.size() == maxHits) {
            break;
        }
        const int cost = backend.CountTokens("- " + hit.text + "\n");
        if (used + cost > capTokens) {
            continue;
        }
        used += cost;
        hit.tokens = cost;
        kept.push_back(std::move(hit));
    }
    return kept;
}
```

- [ ] **Step 6: Query with the scene and the previous digest**

In `src/turn/Session.cpp`, replace

```cpp
    // 1. Retrieve. The query is cut like embedded documents, so a very long input still fits the embedding context.
    std::vector<RetrievalHit> hits = _index.Retrieve(core::CutUtf8(input, EMBED_TEXT_CHARS), ALL_SOURCES,
                                                     _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);
```

with

```cpp
    // 1. Retrieve, with the input, the scene and the previous turn's digest. RetrievalQuery::Text cuts the query like
    // embedded documents, input first, so a very long input still fits the embedding context.
    RetrievalQuery query;
    query.input = input;
    query.scene = SceneOf(_world, state);
    if (turn > 1) {
        const std::optional<core::TurnRecord> previous = _store.Turn(turn - 1);
        MORPHEUS_REQUIRE(previous.has_value(), "PlayTurn: the previous turn is not in the store");
        query.digest = previous->digest;
    }
    std::vector<RetrievalHit> hits =
        _index.Retrieve(query, ALL_SOURCES, _settings.budget.retrievalCap, RETRIEVAL_MAX_HITS, *_pBackend);
```

In the lookup loop, replace

```cpp
            // The query is cut like the automatic one above.
            for (RetrievalHit& hit : _index.Retrieve(core::CutUtf8(lookup.query, EMBED_TEXT_CHARS), source, capLeft,
                                                     LOOKUP_MAX_HITS, *_pBackend)) {
```

with

```cpp
            // A lookup searches with its own query only (RetrievalQuery::Text cuts it like the automatic one).
            for (RetrievalHit& hit :
                 _index.Retrieve(RetrievalQuery{lookup.query}, source, capLeft, LOOKUP_MAX_HITS, *_pBackend)) {
```

In the commit step, replace

```cpp
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back({{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}});
    }
```

with

```cpp
    telemetry["retrieval"] = nlohmann::json::array(); // The hits that made it into the prompt, and why they matched.
    for (const RetrievalHit& hit : hits) {
        telemetry["retrieval"].push_back(
            {{"source", kindText(hit.kind)}, {"ref", hit.refId}, {"score", hit.score}, {"reasons", hit.reasons}});
    }
    telemetry["scene"] = query.scene; // The scene values the automatic query searched with.
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[retrieval],[session]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass. The replays' queries now carry digests (and the sandcrawler scene), which changes only which hits reach the prompts, not the committed turns the replay checks.

- [ ] **Step 8: Commit**

```bash
git add tests/SceneWorld.h src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/Session.cpp tests/turn/RetrievalTests.cpp tests/turn/SessionTests.cpp
git commit -m "Retrieve with the scene and the previous turn's digest" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
