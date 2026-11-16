#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <map>
#include <optional>
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

TEST_CASE("Host data carries lore scopes and conditions; without them it hashes as before", "[hosted][lore]")
{
    // An entry with both round-trips exactly, and the others write neither key.
    nlohmann::json withLore = test::ReadFixture("open.json");
    withLore["lore"][2]["scopes"] = {"hulda", "inn"};
    withLore["lore"][2]["requires"] = {{"pointer", "/quests/MG03"}, {"min", 40}, {"max", 60.5}, {"equals", 55}};
    withLore["lore"][3]["requires"] = {{"pointer", "/gate"}, {"equals", {{"state", "shut"}}}};
    const turn::HostData data = turn::HostDataFromJson(withLore);
    CHECK(data.lore[2].scopes == std::vector<std::string>{"hulda", "inn"});
    REQUIRE(data.lore[2].condition.has_value());
    CHECK(data.lore[2].condition->pointer == "/quests/MG03");
    CHECK(data.lore[2].condition->min == 40);
    CHECK(data.lore[2].condition->max == 60.5);
    CHECK(data.lore[2].condition->equals == nlohmann::json(55));
    REQUIRE(data.lore[3].condition.has_value());
    CHECK_FALSE(data.lore[3].condition->min.has_value());
    CHECK(data.lore[3].condition->equals == R"({"state": "shut"})"_json);
    CHECK(data.lore[0].scopes.empty());
    CHECK_FALSE(data.lore[0].condition.has_value());
    CHECK(turn::HostDataToJson(data) == withLore);
    CHECK(turn::HostDataHash(data) != turn::HostDataHash(test::FixtureHostData()));

    // Host data without them writes neither key and keeps its hash.
    const nlohmann::json plain = turn::HostDataToJson(test::FixtureHostData());
    for (const nlohmann::json& entry : plain.at("lore")) {
        CHECK_FALSE(entry.contains("scopes"));
        CHECK_FALSE(entry.contains("requires"));
    }
    CHECK(turn::HostDataHash(test::FixtureHostData()) == "35856ce76ac0af2e");
    // An empty scopes array is no scopes.
    nlohmann::json emptyScopes = test::ReadFixture("open.json");
    emptyScopes["lore"][2]["scopes"] = nlohmann::json::array();
    CHECK(turn::HostDataHash(turn::HostDataFromJson(emptyScopes)) == turn::HostDataHash(test::FixtureHostData()));

    // Each fault names its pointer. faultOf puts `value` at the entry's key and reads the data.
    auto faultOf = [](const char* key, const nlohmann::json& value) -> std::string {
        nlohmann::json json = test::ReadFixture("open.json");
        json["lore"][2][key] = value;
        try {
            turn::HostDataFromJson(json);
        } catch (const core::DataError& error) {
            return error.what();
        }
        return "";
    };
    const std::string scopes = "host data at /lore/2/scopes: 'scopes' must be an array of non-empty strings";
    CHECK(faultOf("scopes", "inn") == scopes);
    CHECK(faultOf("scopes", R"(["inn", ""])"_json) == scopes);
    CHECK(faultOf("scopes", R"(["inn", 3])"_json) == scopes);
    CHECK(faultOf("requires", "/quests/MG03") == "host data at /lore/2/requires: 'requires' must be an object");
    CHECK(faultOf("requires", R"({"pointer": "/a", "min": 1, "max_by": 2})"_json) ==
          "host data at /lore/2/requires/max_by: unknown key 'max_by'");
    const std::string pointer = "host data at /lore/2/requires/pointer: 'pointer' must be a JSON pointer string";
    CHECK(faultOf("requires", R"({"min": 1})"_json) == pointer);
    CHECK(faultOf("requires", R"({"pointer": 4, "min": 1})"_json) == pointer);
    CHECK(faultOf("requires", R"({"pointer": "/a"})"_json) ==
          "host data at /lore/2/requires: 'requires' needs a comparison: 'min', 'max' or 'equals'");
    CHECK(faultOf("requires", R"({"pointer": "/a", "min": "40"})"_json) ==
          "host data at /lore/2/requires/min: 'min' must be a number");
    CHECK(faultOf("requires", R"({"pointer": "/a", "max": null})"_json) ==
          "host data at /lore/2/requires/max: 'max' must be a number");
}

TEST_CASE("Opening host data checks lore scopes and conditions", "[hosted][lore]")
{
    // An always-on entry with scopes.
    turn::HostData data = test::FixtureHostData();
    data.lore[0].scopes = {"inn"};
    CHECK(OpenError(data) == "lore at /0/scopes: an always-on entry is in every speaker's prompt: scopes apply to "
                             "retrieved entries");
    // Scopes on a retrieved entry, and a condition on an always-on entry, are fine.
    data = test::FixtureHostData();
    data.lore[2].scopes = {"inn"};
    data.lore[0].condition = core::LoreCondition{"/quests/MG03", 40, std::nullopt, std::nullopt};
    CHECK(OpenError(data) == "");
    CHECK(turn::HostedWorld(data).Lore()[2].scopes == std::vector<std::string>{"inn"});

    // A pointer that is not a JSON pointer.
    data = test::FixtureHostData();
    data.lore[2].condition = core::LoreCondition{"quests/MG03", 40, std::nullopt, std::nullopt};
    CHECK_THAT(OpenError(data),
               ContainsSubstring("lore at /2/requires/pointer: 'quests/MG03' is not a JSON pointer: "));
}

TEST_CASE("Observe reads an actor's knows", "[hosted][lore]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    nlohmann::json json = test::FixtureObservation();
    json.at("actors").at(0)["knows"] = {"inn", "mages guild"};
    json.at("actors").at(1)["knows"] = nlohmann::json::array();
    const turn::Observation observation = world.Observe(json);
    CHECK(observation.actors[0].knows == std::vector<std::string>{"inn", "mages guild"});
    CHECK(observation.actors[1].knows.empty());
    CHECK(observation.actors[2].knows.empty()); // Absent: none.
}

TEST_CASE("Host data carries the host's prompt texts; without them it hashes as before packs", "[hosted][prompts]")
{
    // Without a pack the JSON form has no "prompts" and the hash is the one open.json had before packs existed.
    const turn::HostData plain = test::FixtureHostData();
    CHECK(plain.prompts.empty());
    CHECK_FALSE(turn::HostDataToJson(plain).contains("prompts"));
    CHECK(turn::HostDataHash(plain) == "35856ce76ac0af2e");
    // An empty pack is no pack.
    nlohmann::json emptyPack = test::ReadFixture("open.json");
    emptyPack["prompts"] = nlohmann::json::object();
    CHECK(turn::HostDataHash(turn::HostDataFromJson(emptyPack)) == turn::HostDataHash(plain));

    // A pack round-trips with only the texts sent, and changes the hash.
    nlohmann::json withPack = test::ReadFixture("open.json");
    withPack["prompts"] = {{"speak", "Talk, {speaker}."}, {"turn_cue", "({speaker} goes on.)"}};
    const turn::HostData data = turn::HostDataFromJson(withPack);
    CHECK(data.prompts ==
          std::map<std::string, std::string>{{"speak", "Talk, {speaker}."}, {"turn_cue", "({speaker} goes on.)"}});
    CHECK(turn::HostDataToJson(data) == withPack);
    CHECK(turn::HostDataHash(data) != turn::HostDataHash(plain));

    // A wrong shape names its pointer.
    nlohmann::json notObject = test::ReadFixture("open.json");
    notObject["prompts"] = "speak";
    CHECK_THROWS_WITH(turn::HostDataFromJson(notObject),
                      "host data at /prompts: 'prompts' must be a JSON object of texts");
    nlohmann::json notString = test::ReadFixture("open.json");
    notString["prompts"] = {{"speak", 3}};
    CHECK_THROWS_WITH(turn::HostDataFromJson(notString), "host data at /prompts/speak: a prompt text must be a string");

    // HostedWorld checks the content and keeps the pack: the sent texts over the defaults.
    const turn::HostedWorld world(data);
    CHECK(world.Prompts().speak == "Talk, {speaker}.");
    CHECK(world.Prompts().turnCue == "({speaker} goes on.)");
    CHECK(world.Prompts().resolve == std::string(turn::HOSTED_RESOLVE_INSTRUCTION));
    CHECK(turn::HostedWorld(plain).Prompts().speak == std::string(turn::HOSTED_SPEAK_INSTRUCTION));
    turn::HostData bad = test::FixtureHostData();
    bad.prompts = {{"speak", "Talk to {location}."}};
    CHECK(OpenError(bad) == "prompts/speak: unknown variable {location}; this text takes {speaker}, {player}");
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
    const turn::Participants participants = {
        {test::HULDA, "Hulda", ""}, {test::AELA, "Aela", ""}, {test::AELA, "Aela", ""}};
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
    const std::string knows = "'knows' must be an array of non-empty strings";
    CHECK(ObserveError(world, withActor(0, "knows", "inn")) == "observation at /actors/0/knows: " + knows);
    CHECK(ObserveError(world, withActor(0, "knows", R"(["inn", ""])"_json)) ==
          "observation at /actors/0/knows: " + knows);
    CHECK(ObserveError(world, withActor(1, "knows", R"(["inn", 3])"_json)) ==
          "observation at /actors/1/knows: " + knows);
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

TEST_CASE("Adjudicate refuses a number the JSON parser cannot hold instead of throwing", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const nlohmann::json schema = world.ProposalSchema(world.Observe(test::FixtureObservation()), HuldaAndAela(),
                                                       world.ActionIds(), test::HULDA);
    core::Adjudication overflow;
    CHECK_NOTHROW(overflow = world.Adjudicate(schema, R"({"action":"wait","params":{},"x":1e999})"));
    CHECK_FALSE(overflow.requested);
    REQUIRE(overflow.reasons.size() == 1);
    CHECK(overflow.reasons[0].starts_with("the output is not valid JSON: "));
}

TEST_CASE("An empty allowedActions list is named as such, and a minLength reaches the proposal", "[hosted]")
{
    const turn::HostedWorld world(test::FixtureHostData());
    const turn::Observation observation = world.Observe(test::FixtureObservation());
    CHECK_THROWS_WITH(world.ProposalSchema(observation, HuldaAndAela(), {}, test::HULDA),
                      "allowed_actions: the list names no action");

    const turn::HostedWorld naming(WithCatalog(R"({"actions": [{"id": "name", "params": {
        "label": {"type": "string", "maxLength": 10, "minLength": 2}}}]})"));
    const nlohmann::json schema =
        naming.ProposalSchema(naming.Observe(test::FixtureObservation()), HuldaAndAela(), {"name"}, test::HULDA);
    CHECK(schema.at("oneOf")[0].at("properties").at("params").at("properties").at("label") ==
          R"({"type": "string", "maxLength": 10, "minLength": 2})"_json);
}

TEST_CASE("A host clock is read from every observation, and refused when missing or not a number", "[hosted][clock]")
{
    turn::HostData data = test::FixtureHostData();
    CHECK_FALSE(turn::HostedWorld(data).HasClock());
    CHECK_FALSE(turn::HostedWorld(data).Observe(test::FixtureObservation()).clock.has_value());

    data.clock = "/game_time/days_passed";
    const turn::HostedWorld world(data);
    CHECK(world.HasClock());
    const std::optional<double> clock = world.Observe(test::FixtureObservation()).clock;
    REQUIRE(clock.has_value());
    CHECK(*clock == test::FixtureObservation().at("game_time").at("days_passed").get<double>());

    nlohmann::json missing = test::FixtureObservation();
    missing.at("game_time").erase("days_passed");
    CHECK_THROWS_WITH(world.Observe(missing), ContainsSubstring("/game_time/days_passed") &&
                                                  ContainsSubstring("expected the clock, a number"));
    nlohmann::json text = test::FixtureObservation();
    text["game_time"]["days_passed"] = "noon";
    CHECK_THROWS_AS(world.Observe(text), core::DataError);
}

TEST_CASE("A host clock must be a JSON pointer, and round-trips through the host data's JSON", "[hosted][clock]")
{
    nlohmann::json open = test::ReadFixture("open.json");
    open["clock"] = "game_time/days_passed"; // No leading '/'.
    CHECK_THROWS_WITH(turn::HostedWorld(turn::HostDataFromJson(open)), ContainsSubstring("/clock"));
    open["clock"] = 7;
    CHECK_THROWS_WITH(turn::HostDataFromJson(open), ContainsSubstring("/clock"));
    // An empty string would silently mean no clock (a run without one omits the key): refused.
    open["clock"] = "";
    CHECK_THROWS_AS(turn::HostDataFromJson(open), core::DataError);
    CHECK_THROWS_WITH(turn::HostDataFromJson(open), ContainsSubstring("/clock"));
    open["clock"] = "/game_time/days_passed";
    const turn::HostData data = turn::HostDataFromJson(open);
    CHECK(data.clock == "/game_time/days_passed");
    CHECK(turn::HostDataToJson(data).at("clock") == "/game_time/days_passed");
    CHECK_FALSE(turn::HostDataToJson(test::FixtureHostData()).contains("clock"));
}

TEST_CASE("Observe refuses an actor whose id is the reserved 'decide'", "[hosted][decide]")
{
    const turn::HostedWorld world(WithOpenSchema());
    nlohmann::json observation = test::FixtureObservation();
    observation["actors"][1]["id"] = "decide";
    CHECK(ObserveError(world, observation) ==
          "observation at /actors/1/id: 'decide' is reserved: it asks the engine to choose the speaker");
    CHECK(std::string(turn::DECIDE_SPEAKER) == "decide");
}
