#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Errors.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

StateSchema TestSchema()
{
    return StateSchema::FromJson(nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {
                "creatable": true,
                "fields": {
                    "disposition": {"type": "enum", "values": ["wary", "friendly"]},
                    "health": {"type": "int", "min": 0, "max": 5, "default": 5}
                }
            }
        }
    })"));
}

nlohmann::json TestState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// Applies one change, given as JSON text, to TestState().
ApplyResult ApplySingle(const std::string& changeJson)
{
    return ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({nlohmann::json::parse(changeJson)}));
}

const nlohmann::json& PlayerField(const ApplyResult& result, const std::string& field)
{
    return result.state.at("player").at("fields").at(field);
}

} // namespace

TEST_CASE("set replaces a field with a valid value", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "set", "path": "player.location", "value": "bridge"})");
    REQUIRE(result.rejected.empty());
    REQUIRE(result.applied.size() == 1);
    CHECK(result.applied.at(0).change.path == "player.location");
    CHECK(PlayerField(result, "location") == "bridge");
}

TEST_CASE("set rejects anything the schema does not allow and leaves state untouched", "[changes]")
{
    const ApplyResult outOfRange = ApplySingle(R"({"op": "set", "path": "player.health", "value": 11})");
    REQUIRE(outOfRange.rejected.size() == 1);
    CHECK_THAT(outOfRange.rejected.at(0).reason, ContainsSubstring("0..10"));
    CHECK(outOfRange.state == TestState());

    CHECK(ApplySingle(R"({"op": "set", "path": "player.mood", "value": "angry"})").rejected.size() == 1);
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "ghost.health", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("unknown entity"));
    CHECK_THAT(ApplySingle(R"({"op": "set", "path": "player.wings", "value": 1})").rejected.at(0).reason,
               ContainsSubstring("no field"));
    CHECK(ApplySingle(R"({"op": "set", "path": "player.inventory.rope", "value": 3})").rejected.size() == 1);
}

TEST_CASE("add on an int field clamps to the field range", "[changes]")
{
    const ApplyResult up = ApplySingle(R"({"op": "add", "path": "player.health", "value": 5})");
    CHECK(PlayerField(up, "health") == 10);
    CHECK(up.applied.at(0).note == "clamped to 10");

    const ApplyResult down = ApplySingle(R"({"op": "add", "path": "player.health", "value": -2})");
    CHECK(PlayerField(down, "health") == 5);
    CHECK(down.applied.at(0).note.empty());
}

TEST_CASE("add and remove on a list field", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "bruised"})"), "conditions") ==
          nlohmann::json::array({"tired", "bruised"}));

    const ApplyResult duplicate = ApplySingle(R"({"op": "add", "path": "player.conditions", "value": "tired"})");
    CHECK(PlayerField(duplicate, "conditions") == nlohmann::json::array({"tired"}));
    CHECK(duplicate.applied.at(0).note == "already present");

    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "tired"})"), "conditions") ==
          nlohmann::json::array());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": "lost"})").rejected.size() == 1);
}

TEST_CASE("add and remove on counter items", "[changes]")
{
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 2})"), "inventory") ==
          nlohmann::json({{"rope", 3}}));
    CHECK(PlayerField(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": 1})"), "inventory") ==
          nlohmann::json({{"rope", 1}, {"water", 1}}));

    const ApplyResult usedUp = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -1})");
    CHECK(PlayerField(usedUp, "inventory") == nlohmann::json::object());
    CHECK(usedUp.applied.at(0).note == "none left");

    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory.water", "value": -1})").rejected.size() == 1);
    CHECK(PlayerField(ApplySingle(R"({"op": "remove", "path": "player.inventory.rope"})"), "inventory") ==
          nlohmann::json::object());
    CHECK(ApplySingle(R"({"op": "remove", "path": "player.inventory.gold"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "add", "path": "player.inventory", "value": 1})").rejected.size() == 1);
}

TEST_CASE("add rejects integers outside the int range instead of wrapping or overflowing", "[changes]")
{
    const auto addTo = [](const std::string& path, const nlohmann::json& value) {
        const nlohmann::json change = {{"op", "add"}, {"path", path}, {"value", value}};
        return ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({change}));
    };

    const std::vector<nlohmann::json> badValues = {
        nlohmann::json(std::numeric_limits<long long>::max()),
        nlohmann::json(std::numeric_limits<long long>::min()),
        nlohmann::json(std::numeric_limits<std::uint64_t>::max()),
        nlohmann::json(2.5),
    };
    for (const nlohmann::json& value : badValues) {
        CAPTURE(value.dump());
        const ApplyResult result = addTo("player.health", value);
        REQUIRE(result.rejected.size() == 1);
        CHECK(result.applied.empty());
        CHECK_THAT(result.rejected.at(0).reason, ContainsSubstring("int range"));
        CHECK(result.state == TestState());
    }

    // Parsed from text, as it arrives from a model.
    const ApplyResult parsedMax = ApplySingle(R"({"op": "add", "path": "player.health", "value": 18446744073709551615})");
    REQUIRE(parsedMax.rejected.size() == 1);
    CHECK(parsedMax.state == TestState());

    const ApplyResult clamped = ApplySingle(R"({"op": "add", "path": "player.health", "value": 2147483647})");
    REQUIRE(clamped.rejected.empty());
    CHECK(PlayerField(clamped, "health") == 10);
    CHECK(clamped.applied.at(0).note == "clamped to 10");
}

TEST_CASE("add on a counter item rejects out-of-range values and counts above INT_MAX", "[changes]")
{
    const ApplyResult overflow = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 2147483647})");
    REQUIRE(overflow.rejected.size() == 1);
    CHECK_THAT(overflow.rejected.at(0).reason, ContainsSubstring("would exceed 2147483647"));
    CHECK(overflow.state == TestState());

    const ApplyResult wrapped =
        ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 18446744073709551615})");
    REQUIRE(wrapped.rejected.size() == 1);
    CHECK(wrapped.state == TestState());

    const ApplyResult atLimit = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": 2147483646})");
    REQUIRE(atLimit.rejected.empty());
    CHECK(PlayerField(atLimit, "inventory") == nlohmann::json({{"rope", 2147483647}}));
}

TEST_CASE("remove with a non-string value on a list is rejected, not thrown", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "remove", "path": "player.conditions", "value": {"x": 1}})");
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.state == TestState());
}

TEST_CASE("create adds entities of creatable types, filling defaults", "[changes]")
{
    const ApplyResult result = ApplySingle(
        R"({"op": "create", "path": "npc:marta", "value": {"type": "npc", "fields": {"disposition": "friendly"}}})");
    REQUIRE(result.rejected.empty());
    CHECK(result.state.at("npc:marta") ==
          nlohmann::json::parse(R"({"type": "npc", "fields": {"disposition": "friendly", "health": 5}})"));

    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "hero", "value": {"type": "character"}})").rejected.at(0).reason,
               ContainsSubstring("cannot be created"));
    CHECK_THAT(ApplySingle(R"({"op": "create", "path": "player", "value": {"type": "npc"}})").rejected.at(0).reason,
               ContainsSubstring("already exists"));
    CHECK(ApplySingle(R"({"op": "create", "path": "Bad.Id", "value": {"type": "npc"}})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "create", "path": "npc:x", "value": {"type": "npc", "fields": {"health": 9}}})")
              .rejected.size() == 1);
}

TEST_CASE("changes see the effects of earlier changes in the same batch", "[changes]")
{
    const nlohmann::json changes = nlohmann::json::parse(R"([
        {"op": "create", "path": "npc:marta", "value": {"type": "npc"}},
        {"op": "set", "path": "npc:marta.disposition", "value": "friendly"},
        {"op": "set", "path": "player.health", "value": 99},
        {"op": "set", "path": "player.mood", "value": "tense"}
    ])");
    const ApplyResult result = ApplyChanges(TestSchema(), TestState(), changes);

    CHECK(result.applied.size() == 3);
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected.at(0).raw == changes.at(2));
    CHECK(result.state.at("npc:marta").at("fields").at("disposition") == "friendly");
    CHECK(result.state.at("player").at("fields").at("mood") == "tense");
}

TEST_CASE("malformed changes are rejected, not thrown", "[changes]")
{
    CHECK_THAT(ApplySingle(R"({"path": "player.health", "value": 1})").rejected.at(0).reason, ContainsSubstring("op"));
    CHECK(ApplySingle(R"({"op": "explode", "path": "player.health"})").rejected.size() == 1);
    CHECK(ApplySingle(R"({"op": "set", "path": "", "value": 1})").rejected.size() == 1);
    CHECK(ApplySingle(R"("not an object")").rejected.size() == 1);
}

TEST_CASE("ApplyChanges requires an array: anything else is an engine bug", "[changes]")
{
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), TestState(), nlohmann::json::object()), ContractError);
}

TEST_CASE("ChangeToJson round-trips through ParseChange", "[changes]")
{
    const Change change{ChangeOp::Add, "player.health", -2, "fell"};
    std::string error;
    const std::optional<Change> parsed = ParseChange(ChangeToJson(change), error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->op == ChangeOp::Add);
    CHECK(parsed->path == "player.health");
    CHECK(parsed->value == -2);
    CHECK(parsed->reason == "fell");
}

TEST_CASE("add accepts INT_MIN as a negative delta", "[changes]")
{
    // A delta of INT_MIN is in int range: health clamps to its minimum; removing that many ropes is over-removal.
    const ApplyResult health = ApplySingle(R"({"op": "add", "path": "player.health", "value": -2147483648})");
    REQUIRE(health.rejected.empty());
    CHECK(PlayerField(health, "health") == 0);
    CHECK(health.applied.at(0).note == "clamped to 0");

    const ApplyResult rope = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -2147483648})");
    REQUIRE(rope.rejected.size() == 1);
    CHECK_THAT(rope.rejected[0].reason, ContainsSubstring("only 1 'rope' held, cannot remove 2147483648"));
}

TEST_CASE("ApplyChanges throws ContractError when the state breaks the schema contract", "[changes]")
{
    // A player without the declared 'location' field: callers must validate loaded state first.
    nlohmann::json missingField = TestState();
    missingField["player"]["fields"].erase("location");
    const nlohmann::json setLocation =
        nlohmann::json::array({{{"op", "set"}, {"path", "player.location"}, {"value", "dock"}}});
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), missingField, setLocation), ContractError);
    CHECK_THROWS_WITH(ApplyChanges(TestSchema(), missingField, setLocation), ContainsSubstring("location"));

    // An entity of an unknown type.
    nlohmann::json unknownType = TestState();
    unknownType["player"]["type"] = "dragon";
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), unknownType, setLocation), ContractError);

    // An entity with no 'type', or with no 'fields' object.
    nlohmann::json noType = TestState();
    noType["player"].erase("type");
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), noType, setLocation), ContractError);
    nlohmann::json noFields = TestState();
    noFields["player"].erase("fields");
    CHECK_THROWS_AS(ApplyChanges(TestSchema(), noFields, setLocation), ContractError);
}

TEST_CASE("removing more of a counter item than is held is rejected", "[changes]")
{
    const ApplyResult result = ApplySingle(R"({"op": "add", "path": "player.inventory.rope", "value": -3})");
    REQUIRE(result.rejected.size() == 1);
    CHECK_THAT(result.rejected[0].reason, ContainsSubstring("only 1 'rope' held"));
    CHECK(PlayerField(result, "inventory") == nlohmann::json({{"rope", 1}}));
}

TEST_CASE("rejection reasons cut huge proposed values", "[changes]")
{
    const nlohmann::json change = {{"op", "set"}, {"path", "player.health"}, {"value", std::string(1000, 'x')}};
    const ApplyResult result = ApplyChanges(TestSchema(), TestState(), nlohmann::json::array({change}));
    REQUIRE(result.rejected.size() == 1);
    CHECK(result.rejected[0].reason.size() < 200);
}
