### Task 4: `turn::HostedWorld`: host data, observations, the proposal and its adjudication

R5's three operations over a run's host data. The constructor checks the catalog, the observation schema and the lore; `Observe` validates an observation (the fixed core of actors, then the host's schema); `ProposalSchema` / `ProposalGrammar` build the speaker's possible actions for one turn; `Adjudicate` marks the model's proposal requested or refused. The fixtures `open.json` and `observation.json` are written from section 5 of the requirements.

**Files:**
- Create: `src/turn/HostedWorld.h`, `src/turn/HostedWorld.cpp`
- Modify: `src/turn/CMakeLists.txt`
- Create: `tests/hosted/fixtures/open.json`, `tests/hosted/fixtures/observation.json`
- Create: `tests/hosted/HostedTestSupport.h`, `tests/hosted/HostedWorldTests.cpp`

**Interfaces:**
- Consumes: `grammar::Validate`, `grammar::CheckValidationSchema` (Task 1); `core::Participant`, `core::Adjudication` (Task 2).
- Produces (`turn/HostedWorld.h`): `struct turn::HostData { nlohmann::json catalog; nlohmann::json observationSchema; std::vector<core::LoreEntry> lore; }`; `nlohmann::json turn::HostDataToJson(const HostData&)`; `HostData turn::HostDataFromJson(const nlohmann::json&)`; `std::string turn::HostDataHash(const HostData&)`; `struct turn::Actor { std::string id; std::string name; bool isPlayer; std::string bio; }`; `struct turn::Observation { nlohmann::json json; std::vector<Actor> actors; const Actor* Find(std::string_view id) const; const Actor& Player() const; }`; `using turn::Participants = std::vector<core::Participant>`; `class turn::HostedWorld` with `explicit HostedWorld(const HostData& data)`, `const std::vector<core::LoreEntry>& Lore() const`, `std::vector<std::string> ActionIds() const`, `Observation Observe(const nlohmann::json& observation) const`, `static std::vector<Actor> ActorsOf(const nlohmann::json& observation)`, `static void CheckEvents(const std::vector<nlohmann::json>& events)`, `nlohmann::json ProposalSchema(const Observation&, const Participants&, const std::vector<std::string>& allowed, std::string_view speakerId) const`, `std::string ProposalGrammar(...)` (same parameters), `core::Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const`.
- Produces (`tests/hosted/HostedTestSupport.h`, namespace `morpheus::test`): `HostedFixturesDir()`, `ReadFixture(std::string_view name)`, `FixtureHostData()`, `FixtureObservation()`, and the ids `HULDA` (`"0001A6A4"`), `SAADIA` (`"0001B07C"`), `AELA` (`"00000007"`, the player).

- [ ] **Step 1: Write the fixtures and the failing tests**

Create `tests/hosted/fixtures/open.json` (the action catalog of section 5.2, a JSON Schema for the observation of section 5.1 within the extended `Validate` subset, and lore):

```json
{
  "catalog": {
    "actions": [
      {"id": "none"},
      {"id": "follow", "params": {}},
      {"id": "wait", "params": {}},
      {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
      {"id": "give_item", "params": {"item": {"type": "string", "maxLength": 48}}},
      {"id": "end_conversation"}
    ]
  },
  "observation_schema": {
    "type": "object",
    "required": ["game_time", "location", "actors"],
    "properties": {
      "game_time": {
        "type": "object",
        "required": ["days_passed", "hour"],
        "properties": {
          "days_passed": {"type": "number", "minimum": 0},
          "hour": {"type": "integer", "minimum": 0, "maximum": 23}
        }
      },
      "location": {"type": "string"},
      "weather": {"type": "string"},
      "actors": {
        "type": "array",
        "minItems": 1,
        "items": {
          "type": "object",
          "required": ["id", "name", "is_player"],
          "additionalProperties": true,
          "properties": {
            "id": {"type": "string", "minLength": 1},
            "name": {"type": "string", "minLength": 1},
            "is_player": {"type": "boolean"},
            "bio": {"type": "string"},
            "description": {"type": "string"},
            "gender": {"type": "string"},
            "race": {"type": "string"},
            "relationship_rank": {"type": "integer", "minimum": -4, "maximum": 4},
            "in_combat": {"type": "boolean"},
            "hostile": {"type": "boolean"}
          }
        }
      },
      "nearby": {
        "type": "array",
        "items": {
          "type": "object",
          "required": ["id", "name"],
          "additionalProperties": true,
          "properties": {
            "id": {"type": "string"},
            "name": {"type": "string"},
            "distance_m": {"type": "number", "minimum": 0}
          }
        }
      },
      "events": {"type": "array", "items": {"type": "string"}}
    }
  },
  "lore": [
    {"subject": "Whiterun", "data": "A city of the plains, ruled from its keep on the hill.", "always_on": true},
    {"subject": "The Bannered Mare", "data": "The inn of Whiterun, kept by Hulda.", "always_on": true},
    {"subject": "Hulda", "data": "Hulda has kept the Bannered Mare for years and hears every rumour in town.", "always_on": false},
    {"subject": "Saadia", "data": "Saadia came from across the desert and hides from men who hunt her.", "always_on": false}
  ]
}
```

Create `tests/hosted/fixtures/observation.json` (section 5.1's example, with Saadia among the actors):

```json
{
  "game_time": {"days_passed": 213.42, "hour": 14},
  "location": "Whiterun, The Bannered Mare",
  "weather": "clear",
  "actors": [
    {"id": "0001A6A4", "name": "Hulda", "is_player": false, "gender": "female", "race": "Nord",
     "relationship_rank": 1, "in_combat": false, "hostile": false, "voice_type": "FemaleEvenToned",
     "equipment": {"body": "Barkeeper's Clothes"}, "bio": "Hulda keeps the Bannered Mare."},
    {"id": "0001B07C", "name": "Saadia", "is_player": false, "gender": "female", "race": "Redguard",
     "relationship_rank": 0, "in_combat": false, "hostile": false, "bio": "Saadia serves at the inn."},
    {"id": "00000007", "name": "Aela", "is_player": true, "description": "A huntress."}
  ],
  "nearby": [{"id": "0001B07D", "name": "Mikael", "distance_m": 6.5}],
  "events": ["Aela picked up an Iron Sword.", "Hulda started following Aela."]
}
```

Create `tests/hosted/HostedTestSupport.h`:

```cpp
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";

// tests/hosted/fixtures in the source tree: host data and requests written from section 5 of the integration
// requirements. The host's recorded fixtures can replace them later.
inline std::filesystem::path HostedFixturesDir()
{
    return RepoDir() / "tests" / "hosted" / "fixtures";
}

// The fixture `name`, parsed. A fixture that cannot be read is a broken test: it throws.
inline nlohmann::json ReadFixture(std::string_view name)
{
    std::ifstream stream(HostedFixturesDir() / name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFixture: cannot open '" + std::string(name) + "'");
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

// open.json: the host data a host sends when it opens a run.
inline turn::HostData FixtureHostData()
{
    return turn::HostDataFromJson(ReadFixture("open.json"));
}

// observation.json: Hulda and Saadia at the inn, and Aela, the player.
inline nlohmann::json FixtureObservation()
{
    return ReadFixture("observation.json");
}

} // namespace morpheus::test
```

Create `tests/hosted/HostedWorldTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "grammar/Check.h"
#include "grammar/Schema.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr char BACKSLASH = '\\';

// The fixture's conversation: Hulda, and Aela, the player.
turn::Participants HuldaAndAela()
{
    return {{test::HULDA, "Hulda", "Hulda keeps the Bannered Mare."}, {test::AELA, "Aela", ""}};
}

// The fixture host data with another catalog.
turn::HostData WithCatalog(const std::string& catalog)
{
    turn::HostData data = test::FixtureHostData();
    data.catalog = nlohmann::json::parse(catalog);
    return data;
}

// The fixture host data with a schema that lets any object through, so a test sees the core's faults alone.
turn::HostData WithOpenSchema()
{
    turn::HostData data = test::FixtureHostData();
    data.observationSchema = R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})"_json;
    return data;
}

// The fixture observation without Saadia: Hulda and Aela only.
nlohmann::json WithoutSaadia()
{
    nlohmann::json observation = test::FixtureObservation();
    nlohmann::json& actors = observation.at("actors");
    actors.erase(actors.begin() + 1);
    return observation;
}

// The message of the DataError HostedWorld throws for `data`, or "" when it accepts it.
std::string OpenError(const turn::HostData& data)
{
    try {
        const turn::HostedWorld world(data);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

// The message of the DataError Observe throws for `observation`, or "" when it accepts it.
std::string ObserveError(const turn::HostedWorld& world, const nlohmann::json& observation)
{
    try {
        world.Observe(observation);
    } catch (const core::DataError& error) {
        return error.what();
    }
    return "";
}

// The action ids a proposal schema offers, in order.
std::vector<std::string> OfferedIds(const nlohmann::json& schema)
{
    std::vector<std::string> ids;
    for (const nlohmann::json& branch : schema.at("oneOf")) {
        ids.push_back(branch.at("properties").at("action").at("const").get<std::string>());
    }
    return ids;
}

} // namespace

TEST_CASE("Host data reads and writes its JSON form and hashes its content", "[hosted]")
{
    const nlohmann::json fixture = test::ReadFixture("open.json");
    const turn::HostData data = turn::HostDataFromJson(fixture);
    CHECK(data.catalog == fixture.at("catalog"));
    CHECK(data.observationSchema == fixture.at("observation_schema"));
    REQUIRE(data.lore.size() == 4);
    CHECK(data.lore[0].subject == "Whiterun");
    CHECK(data.lore[0].alwaysOn);
    CHECK_FALSE(data.lore[2].alwaysOn);
    CHECK(turn::HostDataToJson(data) == fixture);

    // The hash follows the content.
    CHECK(turn::HostDataHash(data) == turn::HostDataHash(turn::HostDataFromJson(fixture)));
    CHECK(turn::HostDataHash(data).size() == 16);
    turn::HostData changed = data;
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    CHECK(turn::HostDataHash(changed) != turn::HostDataHash(data));

    // Lore may be absent, and always_on defaults to false.
    CHECK(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}})"_json).lore.empty());
    CHECK_FALSE(turn::HostDataFromJson(
                    R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a", "data": "b"}]})"_json)
                    .lore[0]
                    .alwaysOn);

    // A wrong shape is a DataError naming the pointer.
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"([])"_json), "host data at the root: host data must be a JSON object");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"observation_schema": {}})"_json),
                      "host data at /catalog: missing: the action catalog is required");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}})"_json),
                      "host data at /observation_schema: missing: the observation's JSON Schema is required");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "mods": []})"_json),
                      "host data at /mods: unknown key 'mods'");
    CHECK_THROWS_WITH(turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "lore": {}})"_json),
                      "host data at /lore: 'lore' must be an array");
    CHECK_THROWS_WITH(
        turn::HostDataFromJson(R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a"}]})"_json),
        "host data at /lore/0/data: 'data' must be a string");
    CHECK_THROWS_WITH(turn::HostDataFromJson(
                          R"({"catalog": {}, "observation_schema": {}, "lore": [{"subject": "a", "data": "b", "always_on": "yes"}]})"_json),
                      "host data at /lore/0/always_on: 'always_on' must be true or false");
}

TEST_CASE("The catalog gives the proposal schema of section 3, and its grammar takes the right strings", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    CHECK(world.ActionIds() ==
          std::vector<std::string>{"none", "follow", "wait", "attack", "give_item", "end_conversation"});

    // Hulda speaks: attack's present_non_player target is Saadia alone (never the speaker, never the player).
    const nlohmann::json schema = world.ProposalSchema(observation, HuldaAndAela(), world.ActionIds(), test::HULDA);
    const nlohmann::json expected = R"({"oneOf": [
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "none"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "follow"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "wait"},
            "params": {"type": "object", "properties": {}, "required": []}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "attack"},
            "params": {"type": "object", "properties": {"target": {"enum": ["0001B07C"]}}, "required": ["target"]}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "give_item"},
            "params": {"type": "object", "properties": {"item": {"type": "string", "maxLength": 48}}, "required": ["item"]}}},
        {"type": "object", "required": ["action", "params"], "properties": {"action": {"const": "end_conversation"},
            "params": {"type": "object", "properties": {}, "required": []}}}
    ]})"_json;
    CHECK(schema == expected);

    // The grammar is FromSchema's, and it takes the model's compact JSON only.
    const std::string gbnf = world.ProposalGrammar(observation, HuldaAndAela(), world.ActionIds(), test::HULDA);
    CHECK(gbnf == grammar::FromSchema(expected));
    CHECK(grammar::Check(gbnf, R"({"action":"follow","params":{}})").empty());
    CHECK(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001B07C"}})").empty());
    CHECK(grammar::Check(gbnf, R"({"action":"give_item","params":{"item":"Iron Sword"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"wait","params":{},"intent":"rest"})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"attack","params":{"target":"0001A6A4"}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action":"fly","params":{}})").empty());
    CHECK_FALSE(grammar::Check(gbnf, R"({"action": "none", "params": {}})").empty());
    CHECK_FALSE(
        grammar::Check(gbnf, R"({"action":"give_item","params":{"item":")" + std::string(49, 'x') + R"("}})").empty());
}

TEST_CASE("Each actor scope offers its actors, never the speaker, each once", "[hosted]")
{
    const turn::HostedWorld world(WithCatalog(R"({"actions": [
        {"id": "greet", "params": {"whom": {"type": "actor", "scope": "present"}}},
        {"id": "attack", "params": {"target": {"type": "actor", "scope": "present_non_player"}}},
        {"id": "beckon", "params": {"companion": {"type": "actor", "scope": "participant"}}}]})"));
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    // Hulda and Aela take part; Aela is listed twice to show the enum keeps each id once.
    const turn::Participants participants = {{test::HULDA, "Hulda", ""}, {test::AELA, "Aela", ""}, {test::AELA, "Aela", ""}};
    auto targets = [](const nlohmann::json& schema, std::size_t branch, const char* param) {
        return schema.at("oneOf")[branch].at("properties").at("params").at("properties").at(param).at("enum");
    };

    // Hulda speaks: present is Saadia and Aela, in observation order; present_non_player is Saadia; participant is
    // Aela.
    const nlohmann::json hulda = world.ProposalSchema(observation, participants, world.ActionIds(), test::HULDA);
    REQUIRE(hulda.at("oneOf").size() == 3);
    CHECK(targets(hulda, 0, "whom") == nlohmann::json::array({test::SAADIA, test::AELA}));
    CHECK(targets(hulda, 1, "target") == nlohmann::json::array({test::SAADIA}));
    CHECK(targets(hulda, 2, "companion") == nlohmann::json::array({test::AELA}));

    // Saadia speaks: Hulda is present, and a participant.
    const nlohmann::json saadia = world.ProposalSchema(observation, participants, world.ActionIds(), test::SAADIA);
    CHECK(targets(saadia, 0, "whom") == nlohmann::json::array({test::HULDA, test::AELA}));
    CHECK(targets(saadia, 1, "target") == nlohmann::json::array({test::HULDA}));
    CHECK(targets(saadia, 2, "companion") == nlohmann::json::array({test::HULDA, test::AELA}));
}

TEST_CASE("An action without a target this turn is left out, and allowedActions narrows the catalog", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Observation alone = world.Observe(WithoutSaadia());
    // Only Aela, the player, is there besides Hulda: attack's present_non_player target has no one.
    CHECK(OfferedIds(world.ProposalSchema(alone, HuldaAndAela(), world.ActionIds(), test::HULDA)) ==
          std::vector<std::string>{"none", "follow", "wait", "give_item", "end_conversation"});

    // allowedActions keeps the catalog's order and leaves the rest out.
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    CHECK(OfferedIds(world.ProposalSchema(observation, HuldaAndAela(), {"attack", "follow"}, test::HULDA)) ==
          std::vector<std::string>{"follow", "attack"});

    // An unknown or repeated id, or nothing left to do, is the host's DataError naming the field.
    CHECK_THROWS_WITH(world.ProposalSchema(observation, HuldaAndAela(), {"follow", "fly"}, test::HULDA),
                      "allowed_actions/1: 'fly' is not an action of the catalog");
    CHECK_THROWS_WITH(world.ProposalSchema(observation, HuldaAndAela(), {"follow", "follow"}, test::HULDA),
                      "allowed_actions/1: 'follow' is listed twice");
    CHECK_THROWS_AS(world.ProposalSchema(alone, HuldaAndAela(), {"attack"}, test::HULDA), core::DataError);
    CHECK_THROWS_WITH(world.ProposalSchema(alone, HuldaAndAela(), {"attack"}, test::HULDA),
                      ContainsSubstring("allowed_actions: no allowed action"));
}

TEST_CASE("The catalog is checked at open, naming the pointer of its first fault", "[hosted]")
{
    CHECK(OpenError(test::FixtureHostData()).empty());
    CHECK(OpenError(WithCatalog(R"([])")) == "catalog at the root: the catalog must be a JSON object with 'actions'");
    CHECK(OpenError(WithCatalog(R"({"actions": [], "version": 2})")) ==
          "catalog at /version: 'version' does not apply to the catalog");
    CHECK(OpenError(WithCatalog(R"({"actions": []})")) == "catalog at /actions: 'actions' must be a non-empty array");
    CHECK(OpenError(WithCatalog(R"({"actions": ["none"]})")) == "catalog at /actions/0: an action must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": ""}]})")) ==
          "catalog at /actions/0/id: an action needs 'id', a non-empty string");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "none"}, {"id": "none"}]})")) ==
          "catalog at /actions/1/id: the id 'none' is repeated: /actions/0 has it too");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "none", "label": "Nothing"}]})")) ==
          "catalog at /actions/0/label: 'label' does not apply to an action");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": []}]})")) ==
          "catalog at /actions/0/params: 'params' must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": 3}}]})")) ==
          "catalog at /actions/0/params/p: a parameter must be a JSON object");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "float"}}}]})")) ==
          "catalog at /actions/0/params/p/type: 'type' must be 'actor', 'string', 'integer' or 'enum'");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "actor", "scope": "nearby"}}}]})")) ==
          "catalog at /actions/0/params/p/scope: 'scope' must be 'present', 'present_non_player' or 'participant'");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "actor", "scope": "present", "maxLength": 3}}}]})")) ==
          "catalog at /actions/0/params/p/maxLength: 'maxLength' does not apply to an actor parameter");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "string"}}}]})")) ==
          "catalog at /actions/0/params/p/maxLength: a string parameter needs 'maxLength', an integer from 0 to 1999");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "string", "maxLength": 4, "minLength": 5}}}]})")) ==
          "catalog at /actions/0/params/p/minLength: 'minLength' must be an integer from 0 to 4 (the 'maxLength')");
    CHECK(OpenError(WithCatalog(
              R"({"actions": [{"id": "x", "params": {"p": {"type": "integer", "minimum": 5, "maximum": 3}}}]})")) ==
          "catalog at /actions/0/params/p: 'minimum' 5 is above 'maximum' 3");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "integer", "maximum": 3}}}]})")) ==
          "catalog at /actions/0/params/p/minimum: an integer parameter needs 'minimum', an integer");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "enum", "values": []}}}]})")) ==
          "catalog at /actions/0/params/p/values: 'values' must be a non-empty array of strings");
    CHECK(OpenError(WithCatalog(R"({"actions": [{"id": "x", "params": {"p": {"type": "enum", "values": ["a", "a"]}}}]})")) ==
          "catalog at /actions/0/params/p/values/1: 'a' is listed twice");

    // Integer and enum parameters reach the proposal as their own schemas, every parameter required.
    const turn::HostedWorld paying(WithCatalog(R"({"actions": [{"id": "pay", "params": {
        "gold": {"type": "integer", "minimum": 1, "maximum": 100},
        "mood": {"type": "enum", "values": ["calm", "angry"]}}}]})"));
    const nlohmann::json pay =
        paying.ProposalSchema(paying.Observe(test::FixtureObservation()), HuldaAndAela(), {"pay"}, test::HULDA);
    const nlohmann::json& params = pay.at("oneOf")[0].at("properties").at("params");
    CHECK(params.at("properties").at("gold") == R"({"type": "integer", "minimum": 1, "maximum": 100})"_json);
    CHECK(params.at("properties").at("mood") == R"({"enum": ["calm", "angry"]})"_json);
    CHECK(params.at("required") == R"(["gold", "mood"])"_json);
}

TEST_CASE("The observation schema is checked at open against Validate's extended subset", "[hosted]")
{
    turn::HostData data = test::FixtureHostData();
    data.observationSchema =
        R"({"type": "object", "required": [], "properties": {"location": {"type": "string", "pattern": "^W"}}})"_json;
    CHECK(OpenError(data) ==
          "observation_schema: schema at /properties/location/pattern: the keyword 'pattern' is not supported");
    data.observationSchema = R"({"type": "object", "required": [], "properties": {"weather": {"type": "null"}}})"_json;
    CHECK(OpenError(data) == "observation_schema: schema at /properties/weather/type: the type 'null' is not supported");
    // Numbers, booleans, optional keys, open objects and unbounded strings and arrays are in the subset: the
    // fixture's schema uses each.
    data.observationSchema = test::FixtureHostData().observationSchema;
    CHECK(OpenError(data).empty());
}

TEST_CASE("The host's lore is checked at open", "[hosted]")
{
    turn::HostData data = test::FixtureHostData();
    data.lore[1].subject = "";
    CHECK(OpenError(data) == "lore at /1/subject: a lore entry needs a subject");
    data = test::FixtureHostData();
    data.lore[3].subject = "Whiterun";
    CHECK(OpenError(data) == "lore at /3/subject: the subject 'Whiterun' is repeated: /0 has it too");
    data = test::FixtureHostData();
    data.lore[0].data = "";
    CHECK(OpenError(data) == "lore at /0/data: a lore entry needs data");
    CHECK(turn::HostedWorld(test::FixtureHostData()).Lore().size() == 4);
}

TEST_CASE("Observe reads the actors and validates the rest against the host's schema", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const nlohmann::json json = test::FixtureObservation();
    const turn::Observation observation = world.Observe(json);
    CHECK(observation.json == json);
    REQUIRE(observation.actors.size() == 3);
    CHECK(observation.actors[0].id == test::HULDA);
    CHECK(observation.actors[0].name == "Hulda");
    CHECK_FALSE(observation.actors[0].isPlayer);
    CHECK(observation.actors[0].bio == "Hulda keeps the Bannered Mare.");
    CHECK(observation.actors[2].bio.empty()); // The player has a description, not a bio.
    CHECK(observation.Player().id == test::AELA);
    REQUIRE(observation.Find(test::SAADIA) != nullptr);
    CHECK(observation.Find(test::SAADIA)->name == "Saadia");
    CHECK(observation.Find("00000000") == nullptr);
    CHECK(turn::HostedWorld::ActorsOf(json).size() == 3);

    // The rest is the host's to describe: each field its schema refuses is a fault, all of them reported.
    nlohmann::json wrong = json;
    wrong["weather"] = 3;
    wrong["mood"] = "tense";
    const std::string message = ObserveError(world, wrong);
    CHECK_THAT(message, ContainsSubstring("observation at /weather: expected a string, got 3"));
    CHECK_THAT(message, ContainsSubstring("; observation at /mood: unexpected key 'mood'"));
}

TEST_CASE("Each fault of the observation core is a DataError naming its pointer", "[hosted]")
{
    const turn::HostedWorld world(WithOpenSchema());
    // The fixture observation with the field `key` of actor `index` set to `value`.
    auto withActor = [](std::size_t index, const char* key, const nlohmann::json& value) {
        nlohmann::json observation = test::FixtureObservation();
        observation.at("actors").at(index)[key] = value;
        return observation;
    };
    CHECK_THAT(ObserveError(world, nlohmann::json::array()),
               ContainsSubstring("observation at the root: the observation must be a JSON object"));
    CHECK(ObserveError(world, R"({"location": "inn"})"_json) ==
          "observation at /actors: the observation needs 'actors', an array");
    nlohmann::json notAnActor = test::FixtureObservation();
    notAnActor.at("actors").push_back(3);
    CHECK(ObserveError(world, notAnActor) == "observation at /actors/3: an actor must be a JSON object");
    CHECK(ObserveError(world, withActor(0, "id", "")) ==
          "observation at /actors/0/id: an actor needs 'id', a non-empty string");
    CHECK(ObserveError(world, withActor(1, "name", 7)) ==
          "observation at /actors/1/name: an actor needs 'name', a non-empty string");
    CHECK(ObserveError(world, withActor(1, "is_player", "no")) ==
          "observation at /actors/1/is_player: an actor needs 'is_player', true or false");
    CHECK(ObserveError(world, withActor(0, "bio", 5)) == "observation at /actors/0/bio: 'bio' must be a string");
    CHECK(ObserveError(world, withActor(2, "id", test::HULDA)) ==
          "observation at /actors/2/id: the id '0001A6A4' is repeated: /actors/0 has it too");
    CHECK(ObserveError(world, withActor(0, "is_player", true)) ==
          "observation at /actors: exactly one actor must be the player, not 2");
    CHECK(ObserveError(world, withActor(2, "is_player", false)) ==
          "observation at /actors: exactly one actor must be the player, not 0");

    // Every fault is reported, joined in order.
    nlohmann::json twoFaults = withActor(0, "id", "");
    twoFaults.at("actors").at(1)["bio"] = 5;
    CHECK(ObserveError(world, twoFaults) == "observation at /actors/0/id: an actor needs 'id', a non-empty string; "
                                            "observation at /actors/1/bio: 'bio' must be a string");
    CHECK_THROWS_AS(turn::HostedWorld::ActorsOf(twoFaults), core::DataError);
}

TEST_CASE("Adjudicate requests a valid action and refuses the model's mistakes with their reasons", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const nlohmann::json schema = world.ProposalSchema(world.Observe(test::FixtureObservation()), HuldaAndAela(),
                                                       world.ActionIds(), test::HULDA);

    const core::Adjudication attack = world.Adjudicate(schema, R"({"action":"attack","params":{"target":"0001B07C"}})");
    CHECK(attack.requested);
    CHECK(attack.actionId == "attack");
    CHECK(attack.params == R"({"target": "0001B07C"})"_json);
    CHECK(attack.reasons.empty());

    // The measured failure the grammar exists for: an "intent" key added to wait.
    const core::Adjudication intent = world.Adjudicate(schema, R"({"action":"wait","params":{},"intent":"rest"})");
    CHECK_FALSE(intent.requested);
    CHECK(intent.actionId.empty());
    CHECK(intent.reasons == std::vector<std::string>{"/intent: unexpected key 'intent'; the keys are 'action', 'params'"});

    const core::Adjudication fly = world.Adjudicate(schema, R"({"action":"fly","params":{}})");
    CHECK_FALSE(fly.requested);
    REQUIRE(fly.reasons.size() == 1);
    CHECK(fly.reasons[0].starts_with("/action: unknown action 'fly'; expected one of: "));

    // A target the turn does not offer: the speaker herself.
    const core::Adjudication self = world.Adjudicate(schema, R"({"action":"attack","params":{"target":"0001A6A4"}})");
    CHECK_FALSE(self.requested);
    CHECK(self.reasons == std::vector<std::string>{R"(/params/target: '0001A6A4' is not one of: "0001B07C")"});

    // Text that does not parse, among it a lone surrogate escape the grammar admits and the JSON parser refuses.
    const core::Adjudication cut = world.Adjudicate(schema, R"({"action":"fol)");
    CHECK_FALSE(cut.requested);
    REQUIRE(cut.reasons.size() == 1);
    CHECK(cut.reasons[0].starts_with("the output is not valid JSON: "));
    const std::string surrogate = std::string(R"({"action":"give_item","params":{"item":")") + BACKSLASH + "ud800\"}}";
    const core::Adjudication lone = world.Adjudicate(schema, surrogate);
    CHECK_FALSE(lone.requested);
    REQUIRE(lone.reasons.size() == 1);
    CHECK(lone.reasons[0].starts_with("the output is not valid JSON: "));
}

TEST_CASE("Events are JSON objects with a text string", "[hosted]")
{
    CHECK_NOTHROW(turn::HostedWorld::CheckEvents(
        {R"({"type": "action_result", "action": "follow", "actor": "0001A6A4", "succeeded": true, "text": "Hulda is now following Aela."})"_json,
         R"({"text": "Rain."})"_json}));
    CHECK_NOTHROW(turn::HostedWorld::CheckEvents({}));
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"text": "a"})"_json, R"("b")"_json}),
                      "events/1: an event must be a JSON object");
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"type": "ambient"})"_json}),
                      "events/0/text: an event needs 'text', a string");
    CHECK_THROWS_WITH(turn::HostedWorld::CheckEvents({R"({"text": 3})"_json}),
                      "events/0/text: an event needs 'text', a string");
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `Cannot open include file: 'turn/HostedWorld.h'` in `tests/hosted/HostedTestSupport.h`.

- [ ] **Step 3: Write HostedWorld**

Create `src/turn/HostedWorld.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/HostedRecord.h"
#include "core/World.h"

namespace morpheus::turn {

// What the host sends once per run (hosted-world spec, section 1): its action catalog and the JSON Schema of its
// observation, both as the host wrote them, and its lore.
struct HostData {
    nlohmann::json catalog = nlohmann::json::object();
    nlohmann::json observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> lore = {};
};

// HostData as a hosted run stores it and as the fixtures write it:
// {"catalog": ..., "observation_schema": ..., "lore": [{"subject": ..., "data": ..., "always_on": ...}]}.
nlohmann::json HostDataToJson(const HostData& data);

// Reads HostDataToJson's form; "lore" may be absent (no lore) and "always_on" defaults to false. Checks the shape only
// (HostedWorld checks the content): throws DataError "host data at <pointer>: <problem>".
HostData HostDataFromJson(const nlohmann::json& json);

// The content hash of `data` (core::ContentHash over HostDataToJson's text) that a hosted run stores: changed host
// data, such as a mod update, has another.
std::string HostDataHash(const HostData& data);

// One actor of an observation, as the engine reads it.
struct Actor {
    std::string id = {};
    std::string name = {};
    bool isPlayer = false;
    std::string bio = {}; // Empty when the observation gives none.
};

// An observation that passed HostedWorld::Observe: the host's JSON as sent, and its actors in their order.
struct Observation {
    nlohmann::json json = nlohmann::json::object();
    std::vector<Actor> actors = {};

    // The actor with `id`, or nullptr when it is not in the observation.
    const Actor* Find(std::string_view id) const;
    // The one actor that is the player. Throws ContractError for an observation without one (Observe refuses those).
    const Actor& Player() const;
};

// A conversation's participants, in join order.
using Participants = std::vector<core::Participant>;

// R5's three operations over the host data of one hosted run (hosted-world spec, section 1): Observe,
// ProposalGrammar and Adjudicate. It never simulates the world: an accepted action is requested, not applied.
class HostedWorld {
private:
    enum class ParamType { Actor, String, Integer, Enum };
    enum class Scope { Present, PresentNonPlayer, Participant };

    // One parameter of a catalog action.
    struct Param {
        std::string name = {};
        ParamType type = ParamType::String;
        Scope scope = Scope::Present;                    // Actor: whose ids a turn offers.
        nlohmann::json schema = nlohmann::json::object(); // String, Integer, Enum: the parameter's proposal schema.
    };

    // One catalog action, its parameters by name.
    struct Action {
        std::string id = {};
        std::vector<Param> params = {};
    };

    std::vector<Action> _actions = {}; // In catalog order.
    nlohmann::json _observationSchema = nlohmann::json::object();
    std::vector<core::LoreEntry> _lore = {};

public:
    // Checks the host data and keeps it. Throws DataError naming the first fault: "catalog at <pointer>: ...",
    // "observation_schema: schema at <pointer>: ..." (the schema must be in grammar::Validate's extended subset) or
    // "lore at /<index>/<field>: ..." (subjects non-empty and unique, data non-empty).
    explicit HostedWorld(const HostData& data);

    const std::vector<core::LoreEntry>& Lore() const;

    // Every action id of the catalog, in order: what a turn allows when the host names no allowed actions.
    std::vector<std::string> ActionIds() const;

    // Validates `observation` and returns it with its parsed core. First the core, which the engine needs whatever the
    // host's schema says: `actors`, an array of objects each with `id` and `name` (non-empty strings), `is_player`
    // (a boolean) and an optional `bio` (a string); ids unique; exactly one player. Then grammar::Validate against the
    // host's schema. Every fault of both is collected; any fault is a DataError
    // "observation at <pointer>: <problem>; observation at ...".
    Observation Observe(const nlohmann::json& observation) const;

    // The core actors of `observation`, read as Observe reads them but without the host's schema, so a stored
    // observation still reads after the host data changed. Throws Observe's DataError for a fault of the core.
    static std::vector<Actor> ActorsOf(const nlohmann::json& observation);

    // Events are JSON objects with a `text` string; their other fields are the host's. Throws DataError
    // "events/<i>: ..." or "events/<i>/text: ..." for the first that is not.
    static void CheckEvents(const std::vector<nlohmann::json>& events);

    // The JSON Schema of the speaker's possible actions this turn (hosted-world spec, section 3): a oneOf over the
    // allowed actions in catalog order, discriminated by "action", each branch {"action": <const id>, "params": {...}}
    // with every parameter required and no extra keys. An actor parameter is an enum of the actor ids its scope gives
    // (present: every actor of the observation; present_non_player: those but the player; participant: the
    // participants present), never the speaker, each once, in observation order; an action whose actor enum would be
    // empty is left out. Throws DataError "allowed_actions/<i>: ..." for an id not in the catalog or listed twice, and
    // "allowed_actions: ..." when no allowed action is left.
    nlohmann::json ProposalSchema(const Observation& observation, const Participants& participants,
                                  const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // grammar::FromSchema of ProposalSchema(...): the compact JSON the resolve call may write.
    std::string ProposalGrammar(const Observation& observation, const Participants& participants,
                                const std::vector<std::string>& allowed, std::string_view speakerId) const;

    // What becomes of the model's proposal: parsed (a parse failure is a refusal: the grammar admits lone surrogate
    // escapes that the JSON parser refuses), then grammar::Validate against `proposalSchema`. A valid proposal is
    // requested with its action id and params; otherwise it is refused, and the reasons are the parse error or each
    // validation error as "<pointer>: <message>". Never throws for the model's text.
    core::Adjudication Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const;
};

} // namespace morpheus::turn
```

Create `src/turn/HostedWorld.cpp`:

```cpp
#include "turn/HostedWorld.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
#include "grammar/Schema.h"

namespace morpheus::turn {

namespace {

// `key` as one reference token of a JSON pointer (RFC 6901): "~" is written "~0" and "/" is written "~1".
std::string PointerToken(std::string_view key)
{
    std::string token;
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

// One fault of an observation: where it is ("" for the observation itself) and what is wrong.
struct Fault {
    std::string pointer = {};
    std::string problem = {};
};

// Reads the observation's core (its actors), adding a Fault for every mistake, and returns the actors read whole, in
// order. Observe and ActorsOf both read the core with it.
std::vector<Actor> ReadActors(const nlohmann::json& observation, std::vector<Fault>& faults)
{
    std::vector<Actor> actors;
    if (!observation.is_object()) {
        faults.push_back({"", "the observation must be a JSON object"});
        return actors;
    }
    if (!observation.contains("actors") || !observation.at("actors").is_array()) {
        faults.push_back({"/actors", "the observation needs 'actors', an array"});
        return actors;
    }
    const nlohmann::json& list = observation.at("actors");
    std::map<std::string, std::size_t> seen; // Actor id -> the index that has it first.
    int players = 0;                         // Actors whose is_player is true.
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = std::format("/actors/{}", i);
        const nlohmann::json& entry = list[i];
        if (!entry.is_object()) {
            faults.push_back({at, "an actor must be a JSON object"});
            continue;
        }
        // A required non-empty string field; "" with a fault when it is missing or wrong.
        auto text = [&](const char* key) {
            const bool good = entry.contains(key) && entry.at(key).is_string() &&
                              !entry.at(key).get_ref<const std::string&>().empty();
            if (!good) {
                faults.push_back({at + "/" + key, std::format("an actor needs '{}', a non-empty string", key)});
                return std::string();
            }
            return entry.at(key).get<std::string>();
        };
        Actor actor;
        actor.id = text("id");
        actor.name = text("name");
        bool whole = !actor.id.empty() && !actor.name.empty();
        if (!entry.contains("is_player") || !entry.at("is_player").is_boolean()) {
            faults.push_back({at + "/is_player", "an actor needs 'is_player', true or false"});
            whole = false;
        } else {
            actor.isPlayer = entry.at("is_player").get<bool>();
            players += actor.isPlayer ? 1 : 0;
        }
        if (entry.contains("bio")) {
            if (entry.at("bio").is_string()) {
                actor.bio = entry.at("bio").get<std::string>();
            } else {
                faults.push_back({at + "/bio", "'bio' must be a string"});
                whole = false;
            }
        }
        // Ids are unique: the engine names actors by them.
        if (!actor.id.empty()) {
            const auto [first, inserted] = seen.emplace(actor.id, i);
            if (!inserted) {
                faults.push_back({at + "/id", std::format("the id '{}' is repeated: /actors/{} has it too", actor.id,
                                                          first->second)});
                whole = false;
            }
        }
        if (whole) {
            actors.push_back(std::move(actor));
        }
    }
    if (players != 1) {
        faults.push_back({"/actors", std::format("exactly one actor must be the player, not {}", players)});
    }
    return actors;
}

// The DataError for an observation's faults: "observation at <pointer>: <problem>", joined by "; ".
core::DataError ObservationError(const std::vector<Fault>& faults)
{
    std::string message;
    for (const Fault& fault : faults) {
        message += std::format("{}observation at {}: {}", message.empty() ? "" : "; ",
                               fault.pointer.empty() ? "the root" : fault.pointer, fault.problem);
    }
    return core::DataError(message);
}

} // namespace

nlohmann::json HostDataToJson(const HostData& data)
{
    nlohmann::json lore = nlohmann::json::array();
    for (const core::LoreEntry& entry : data.lore) {
        lore.push_back({{"subject", entry.subject}, {"data", entry.data}, {"always_on", entry.alwaysOn}});
    }
    return {{"catalog", data.catalog}, {"observation_schema", data.observationSchema}, {"lore", std::move(lore)}};
}

HostData HostDataFromJson(const nlohmann::json& json)
{
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("host data at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    if (!json.is_object()) {
        throw fault("", "host data must be a JSON object");
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "catalog" && it.key() != "observation_schema" && it.key() != "lore") {
            throw fault("/" + PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
        }
    }
    if (!json.contains("catalog")) {
        throw fault("/catalog", "missing: the action catalog is required");
    }
    if (!json.contains("observation_schema")) {
        throw fault("/observation_schema", "missing: the observation's JSON Schema is required");
    }
    HostData data;
    data.catalog = json.at("catalog");
    data.observationSchema = json.at("observation_schema");
    if (!json.contains("lore")) {
        return data;
    }

    // Each lore entry: a subject and data, both strings, and an optional always_on.
    const nlohmann::json& lore = json.at("lore");
    if (!lore.is_array()) {
        throw fault("/lore", "'lore' must be an array");
    }
    for (std::size_t i = 0; i < lore.size(); ++i) {
        const std::string at = std::format("/lore/{}", i);
        const nlohmann::json& entry = lore[i];
        if (!entry.is_object()) {
            throw fault(at, "a lore entry must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "subject" && it.key() != "data" && it.key() != "always_on") {
                throw fault(at + "/" + PointerToken(it.key()), std::format("unknown key '{}'", it.key()));
            }
        }
        if (!entry.contains("subject") || !entry.at("subject").is_string()) {
            throw fault(at + "/subject", "'subject' must be a string");
        }
        if (!entry.contains("data") || !entry.at("data").is_string()) {
            throw fault(at + "/data", "'data' must be a string");
        }
        if (entry.contains("always_on") && !entry.at("always_on").is_boolean()) {
            throw fault(at + "/always_on", "'always_on' must be true or false");
        }
        data.lore.push_back({entry.at("subject").get<std::string>(), entry.at("data").get<std::string>(),
                             entry.value("always_on", false)});
    }
    return data;
}

std::string HostDataHash(const HostData& data)
{
    // The replace handler keeps a string that is not UTF-8 from throwing; the text is the same for the same data.
    core::ContentHash hash;
    hash.Add(HostDataToJson(data).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return hash.Hex();
}

const Actor* Observation::Find(std::string_view id) const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [id](const Actor& actor) { return actor.id == id; });
    return it == actors.end() ? nullptr : &*it;
}

const Actor& Observation::Player() const
{
    const auto it = std::find_if(actors.begin(), actors.end(), [](const Actor& actor) { return actor.isPlayer; });
    MORPHEUS_REQUIRE(it != actors.end(), "Observation::Player: an observation that passed Observe has its player");
    return *it;
}

HostedWorld::HostedWorld(const HostData& data)
    : _observationSchema(data.observationSchema)
    , _lore(data.lore)
{
    // 1. The catalog: {"actions": [...]}, each action an id and its parameters.
    auto fault = [](const std::string& pointer, const std::string& problem) {
        return core::DataError(std::format("catalog at {}: {}", pointer.empty() ? "the root" : pointer, problem));
    };
    const nlohmann::json& catalog = data.catalog;
    if (!catalog.is_object()) {
        throw fault("", "the catalog must be a JSON object with 'actions'");
    }
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        if (it.key() != "actions") {
            throw fault("/" + PointerToken(it.key()), std::format("'{}' does not apply to the catalog", it.key()));
        }
    }
    if (!catalog.contains("actions") || !catalog.at("actions").is_array() || catalog.at("actions").empty()) {
        throw fault("/actions", "'actions' must be a non-empty array");
    }
    const nlohmann::json& actions = catalog.at("actions");
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::string at = std::format("/actions/{}", i);
        const nlohmann::json& entry = actions[i];
        if (!entry.is_object()) {
            throw fault(at, "an action must be a JSON object");
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "id" && it.key() != "params") {
                throw fault(at + "/" + PointerToken(it.key()), std::format("'{}' does not apply to an action", it.key()));
            }
        }
        if (!entry.contains("id") || !entry.at("id").is_string() || entry.at("id").get_ref<const std::string&>().empty()) {
            throw fault(at + "/id", "an action needs 'id', a non-empty string");
        }
        Action action;
        action.id = entry.at("id").get<std::string>();
        for (std::size_t j = 0; j < _actions.size(); ++j) {
            if (_actions[j].id == action.id) {
                throw fault(at + "/id", std::format("the id '{}' is repeated: /actions/{} has it too", action.id, j));
            }
        }

        // Absent params: no parameters. Each parameter is checked against its type's keys.
        if (entry.contains("params")) {
            const nlohmann::json& params = entry.at("params");
            if (!params.is_object()) {
                throw fault(at + "/params", "'params' must be a JSON object");
            }
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string paramAt = at + "/params/" + PointerToken(it.key());
                const nlohmann::json& definition = it.value();
                if (!definition.is_object()) {
                    throw fault(paramAt, "a parameter must be a JSON object");
                }
                Param param;
                param.name = it.key();
                const std::string typeName = definition.contains("type") && definition.at("type").is_string()
                                                 ? definition.at("type").get<std::string>()
                                                 : std::string();
                std::vector<std::string_view> keys; // The keys this type allows.
                std::string what;                   // The type as messages name it.
                if (typeName == "actor") {
                    param.type = ParamType::Actor;
                    keys = {"type", "scope"};
                    what = "an actor";
                } else if (typeName == "string") {
                    param.type = ParamType::String;
                    keys = {"type", "maxLength", "minLength"};
                    what = "a string";
                } else if (typeName == "integer") {
                    param.type = ParamType::Integer;
                    keys = {"type", "minimum", "maximum"};
                    what = "an integer";
                } else if (typeName == "enum") {
                    param.type = ParamType::Enum;
                    keys = {"type", "values"};
                    what = "an enum";
                } else {
                    throw fault(paramAt + "/type", "'type' must be 'actor', 'string', 'integer' or 'enum'");
                }
                for (auto key = definition.begin(); key != definition.end(); ++key) {
                    if (std::find(keys.begin(), keys.end(), key.key()) == keys.end()) {
                        throw fault(paramAt + "/" + PointerToken(key.key()),
                                    std::format("'{}' does not apply to {} parameter", key.key(), what));
                    }
                }

                switch (param.type) {
                case ParamType::Actor: {
                    // Whose ids a turn offers.
                    const std::string scope = definition.contains("scope") && definition.at("scope").is_string()
                                                  ? definition.at("scope").get<std::string>()
                                                  : std::string();
                    if (scope == "present") {
                        param.scope = Scope::Present;
                    } else if (scope == "present_non_player") {
                        param.scope = Scope::PresentNonPlayer;
                    } else if (scope == "participant") {
                        param.scope = Scope::Participant;
                    } else {
                        throw fault(paramAt + "/scope", "'scope' must be 'present', 'present_non_player' or 'participant'");
                    }
                    break;
                }
                case ParamType::String: {
                    // Bounded, so the proposal grammar is: maxLength within llama.cpp's limit, minLength up to it.
                    const std::optional<long long> maxLength =
                        definition.contains("maxLength")
                            ? core::JsonIntInRange(definition.at("maxLength"), 0, grammar::GBNF_MAX_STRING_CHARS)
                            : std::nullopt;
                    if (!maxLength) {
                        throw fault(paramAt + "/maxLength",
                                    std::format("a string parameter needs 'maxLength', an integer from 0 to {}",
                                                grammar::GBNF_MAX_STRING_CHARS));
                    }
                    param.schema = {{"type", "string"}, {"maxLength", *maxLength}};
                    if (definition.contains("minLength")) {
                        const std::optional<long long> minLength =
                            core::JsonIntInRange(definition.at("minLength"), 0, *maxLength);
                        if (!minLength) {
                            throw fault(paramAt + "/minLength",
                                        std::format("'minLength' must be an integer from 0 to {} (the 'maxLength')",
                                                    *maxLength));
                        }
                        param.schema["minLength"] = *minLength;
                    }
                    break;
                }
                case ParamType::Integer: {
                    // Both bounds, in order: the grammar writes the exact range.
                    auto bound = [&](const char* key) -> long long {
                        const std::optional<long long> value =
                            definition.contains(key)
                                ? core::JsonIntInRange(definition.at(key), std::numeric_limits<long long>::min(),
                                                       std::numeric_limits<long long>::max())
                                : std::nullopt;
                        if (!value) {
                            throw fault(paramAt + "/" + key,
                                        std::format("an integer parameter needs '{}', an integer", key));
                        }
                        return *value;
                    };
                    const long long minimum = bound("minimum");
                    const long long maximum = bound("maximum");
                    if (minimum > maximum) {
                        throw fault(paramAt, std::format("'minimum' {} is above 'maximum' {}", minimum, maximum));
                    }
                    param.schema = {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
                    break;
                }
                case ParamType::Enum: {
                    // Strings, each once.
                    const nlohmann::json values = definition.value("values", nlohmann::json());
                    const bool strings =
                        values.is_array() && !values.empty() &&
                        std::all_of(values.begin(), values.end(), [](const nlohmann::json& value) { return value.is_string(); });
                    if (!strings) {
                        throw fault(paramAt + "/values", "'values' must be a non-empty array of strings");
                    }
                    for (std::size_t k = 0; k < values.size(); ++k) {
                        for (std::size_t l = 0; l < k; ++l) {
                            if (values[l] == values[k]) {
                                throw fault(std::format("{}/values/{}", paramAt, k),
                                            std::format("'{}' is listed twice", values[k].get<std::string>()));
                            }
                        }
                    }
                    param.schema = {{"enum", values}};
                    break;
                }
                }
                action.params.push_back(std::move(param));
            }
        }
        _actions.push_back(std::move(action));
    }

    // 2. The observation schema: in Validate's extended subset, so an unsupported keyword fails here, not at a turn.
    try {
        grammar::CheckValidationSchema(data.observationSchema);
    } catch (const core::DataError& error) {
        throw core::DataError(std::format("observation_schema: {}", error.what()));
    }

    // 3. The lore: each entry has a subject, unique, and data.
    for (std::size_t i = 0; i < data.lore.size(); ++i) {
        const core::LoreEntry& entry = data.lore[i];
        if (entry.subject.empty()) {
            throw core::DataError(std::format("lore at /{}/subject: a lore entry needs a subject", i));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (data.lore[j].subject == entry.subject) {
                throw core::DataError(std::format("lore at /{}/subject: the subject '{}' is repeated: /{} has it too",
                                                  i, entry.subject, j));
            }
        }
        if (entry.data.empty()) {
            throw core::DataError(std::format("lore at /{}/data: a lore entry needs data", i));
        }
    }
}

const std::vector<core::LoreEntry>& HostedWorld::Lore() const
{
    return _lore;
}

std::vector<std::string> HostedWorld::ActionIds() const
{
    std::vector<std::string> ids;
    for (const Action& action : _actions) {
        ids.push_back(action.id);
    }
    return ids;
}

Observation HostedWorld::Observe(const nlohmann::json& observation) const
{
    // The core first, then the host's schema; every fault of both is collected.
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    for (const grammar::ValidationError& error : grammar::Validate(observation, _observationSchema)) {
        faults.push_back({error.path, error.message});
    }
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return Observation{observation, std::move(actors)};
}

std::vector<Actor> HostedWorld::ActorsOf(const nlohmann::json& observation)
{
    std::vector<Fault> faults;
    std::vector<Actor> actors = ReadActors(observation, faults);
    if (!faults.empty()) {
        throw ObservationError(faults);
    }
    return actors;
}

void HostedWorld::CheckEvents(const std::vector<nlohmann::json>& events)
{
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!events[i].is_object()) {
            throw core::DataError(std::format("events/{}: an event must be a JSON object", i));
        }
        if (!events[i].contains("text") || !events[i].at("text").is_string()) {
            throw core::DataError(std::format("events/{}/text: an event needs 'text', a string", i));
        }
    }
}

nlohmann::json HostedWorld::ProposalSchema(const Observation& observation, const Participants& participants,
                                           const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    // Every allowed id names an action of the catalog, once.
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        const bool known = std::any_of(_actions.begin(), _actions.end(),
                                       [&](const Action& action) { return action.id == allowed[i]; });
        if (!known) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is not an action of the catalog", i, allowed[i]));
        }
        if (std::find(allowed.begin(), allowed.begin() + static_cast<std::ptrdiff_t>(i), allowed[i]) !=
            allowed.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw core::DataError(std::format("allowed_actions/{}: '{}' is listed twice", i, allowed[i]));
        }
    }

    // The actor ids a scope offers this turn, in observation order: never the speaker, each once.
    auto actorsIn = [&](Scope scope) {
        std::vector<std::string> ids;
        std::set<std::string> taken;
        for (const Actor& actor : observation.actors) {
            if (actor.id == speakerId) {
                continue;
            }
            const bool participates = std::any_of(participants.begin(), participants.end(),
                                                  [&](const core::Participant& p) { return p.id == actor.id; });
            const bool inScope = scope == Scope::Present || (scope == Scope::PresentNonPlayer && !actor.isPlayer) ||
                                 (scope == Scope::Participant && participates);
            if (inScope && taken.insert(actor.id).second) {
                ids.push_back(actor.id);
            }
        }
        return ids;
    };

    // One branch per allowed action the turn can offer, in catalog order.
    nlohmann::json branches = nlohmann::json::array();
    for (const Action& action : _actions) {
        if (std::find(allowed.begin(), allowed.end(), action.id) == allowed.end()) {
            continue;
        }
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        bool offered = true; // False when an actor parameter has no one to name: the action is left out.
        for (const Param& param : action.params) {
            nlohmann::json schema = param.schema;
            if (param.type == ParamType::Actor) {
                const std::vector<std::string> ids = actorsIn(param.scope);
                if (ids.empty()) {
                    offered = false;
                    break;
                }
                schema = {{"enum", ids}};
            }
            properties[param.name] = std::move(schema);
            required.push_back(param.name);
        }
        if (!offered) {
            continue;
        }
        const nlohmann::json paramsSchema = {{"type", "object"}, {"properties", properties}, {"required", required}};
        branches.push_back({{"type", "object"},
                            {"required", nlohmann::json::array({"action", "params"})},
                            {"properties", {{"action", {{"const", action.id}}}, {"params", paramsSchema}}}});
    }
    if (branches.empty()) {
        throw core::DataError("allowed_actions: no allowed action can be taken this turn: each needs an actor its "
                              "scope does not offer");
    }
    return {{"oneOf", std::move(branches)}};
}

std::string HostedWorld::ProposalGrammar(const Observation& observation, const Participants& participants,
                                         const std::vector<std::string>& allowed, std::string_view speakerId) const
{
    return grammar::FromSchema(ProposalSchema(observation, participants, allowed, speakerId));
}

core::Adjudication HostedWorld::Adjudicate(const nlohmann::json& proposalSchema, std::string_view modelText) const
{
    core::Adjudication adjudication;
    // A parse failure is the model's mistake: the grammar admits lone surrogate escapes that the parser refuses.
    nlohmann::json proposal;
    try {
        proposal = nlohmann::json::parse(modelText.begin(), modelText.end());
    } catch (const nlohmann::json::parse_error& error) {
        adjudication.reasons.push_back(std::format("the output is not valid JSON: {}", error.what()));
        return adjudication;
    }
    // Then the proposal schema of the turn: every mistake is a reason.
    for (const grammar::ValidationError& error : grammar::Validate(proposal, proposalSchema)) {
        adjudication.reasons.push_back(std::format("{}: {}", error.path.empty() ? "the output" : error.path, error.message));
    }
    if (!adjudication.reasons.empty()) {
        return adjudication;
    }
    adjudication.requested = true;
    adjudication.actionId = proposal.at("action").get<std::string>();
    adjudication.params = proposal.at("params");
    return adjudication;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    MemoryUpkeep.cpp
```

with

```cmake
    Folding.cpp
    HostedWorld.cpp
    MemoryUpkeep.cpp
```

- [ ] **Step 4: Run the hosted tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: builds with no warnings; `All tests passed` (the store's `[store][hosted]` tests and the eleven new ones).

- [ ] **Step 5: Run the whole fast suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 457` (446 + 11), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/turn/HostedWorld.h src/turn/HostedWorld.cpp src/turn/CMakeLists.txt tests/hosted/fixtures/open.json tests/hosted/fixtures/observation.json tests/hosted/HostedTestSupport.h tests/hosted/HostedWorldTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add turn::HostedWorld: host data checks, observations, the proposal schema and grammar, Adjudicate" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

