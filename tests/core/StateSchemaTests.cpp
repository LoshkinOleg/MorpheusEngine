#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/StateSchema.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

namespace {

nlohmann::json SchemaJson()
{
    return nlohmann::json::parse(R"({
        "entity_types": {
            "character": {
                "fields": {
                    "health": {"type": "int", "min": 0, "max": 10, "default": 10},
                    "mood": {"type": "enum", "values": ["calm", "tense"]},
                    "location": {"type": "string"},
                    "conditions": {"type": "list"},
                    "inventory": {"type": "counter"}
                }
            },
            "npc": {"creatable": true, "fields": {"health": {"type": "int", "min": 0, "max": 5}}}
        }
    })");
}

nlohmann::json ValidState()
{
    return nlohmann::json::parse(R"({
        "player": {"type": "character", "fields": {
            "health": 7, "mood": "calm", "location": "deck", "conditions": ["tired"], "inventory": {"rope": 1}}}
    })");
}

// The schema JSON with one extra field "bad" on the character type.
nlohmann::json SchemaWithBadField(const nlohmann::json& field)
{
    nlohmann::json json = SchemaJson();
    json["entity_types"]["character"]["fields"]["bad"] = field;
    return json;
}

} // namespace

TEST_CASE("StateSchema parses entity types and fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    const EntityTypeDef* pCharacter = schema.FindEntityType("character");
    REQUIRE(pCharacter != nullptr);
    CHECK_FALSE(pCharacter->creatable);
    CHECK(pCharacter->fields.at("health").type == FieldType::Int);
    CHECK(pCharacter->fields.at("health").max == 10);
    CHECK(pCharacter->fields.at("mood").values == std::vector<std::string>{"calm", "tense"});
    CHECK(pCharacter->fields.at("inventory").type == FieldType::Counter);

    REQUIRE(schema.FindEntityType("npc") != nullptr);
    CHECK(schema.FindEntityType("npc")->creatable);
    CHECK(schema.FindEntityType("dragon") == nullptr);
}

TEST_CASE("StateSchema rejects malformed definitions", "[schema]")
{
    CHECK_THROWS_AS(StateSchema::FromJson(nlohmann::json::object()), DataError);
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "float"}})), ContainsSubstring("'bad'"));
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 5}, {"max", 1}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "enum"}, {"values", nlohmann::json::array()}})), DataError);
    CHECK_THROWS_AS(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 3}, {"default", 9}})), DataError);
}

TEST_CASE("StateSchema builds default fields", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());

    CHECK(schema.DefaultFields("character") == nlohmann::json::parse(R"({
        "health": 10, "mood": "calm", "location": "", "conditions": [], "inventory": {}
    })"));
    CHECK(schema.DefaultFields("npc") == nlohmann::json::parse(R"({"health": 0})"));
    CHECK_THROWS_AS(schema.DefaultFields("dragon"), ContractError);
}

TEST_CASE("CheckFieldValue explains invalid values", "[schema]")
{
    FieldDef health;
    health.type = FieldType::Int;
    health.min = 0;
    health.max = 10;
    CHECK(StateSchema::CheckFieldValue(health, 4).empty());
    CHECK_THAT(StateSchema::CheckFieldValue(health, 11), ContainsSubstring("0..10"));
    CHECK_FALSE(StateSchema::CheckFieldValue(health, "4").empty());

    FieldDef conditions;
    conditions.type = FieldType::List;
    CHECK(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "b"})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(conditions, nlohmann::json::array({"a", "a"})).empty());

    FieldDef inventory;
    inventory.type = FieldType::Counter;
    CHECK(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 2}})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 0}})).empty());
}

TEST_CASE("ValidateState accepts valid state and names what is wrong otherwise", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaJson());
    CHECK_NOTHROW(schema.ValidateState(ValidState()));

    nlohmann::json missingField = ValidState();
    missingField["player"]["fields"].erase("inventory");
    CHECK_THROWS_WITH(schema.ValidateState(missingField), ContainsSubstring("inventory"));

    nlohmann::json extraField = ValidState();
    extraField["player"]["fields"]["wings"] = 2;
    CHECK_THROWS_WITH(schema.ValidateState(extraField), ContainsSubstring("wings"));

    nlohmann::json badValue = ValidState();
    badValue["player"]["fields"]["health"] = 99;
    CHECK_THROWS_WITH(schema.ValidateState(badValue), ContainsSubstring("health"));

    nlohmann::json badType = ValidState();
    badType["player"]["type"] = "dragon";
    CHECK_THROWS_WITH(schema.ValidateState(badType), ContainsSubstring("dragon"));

    nlohmann::json badId = ValidState();
    badId["Bad.Id"] = badId["player"];
    CHECK_THROWS_AS(schema.ValidateState(badId), DataError);
}

TEST_CASE("Entity ids never contain the path separator", "[schema]")
{
    CHECK(StateSchema::IsValidEntityId("player"));
    CHECK(StateSchema::IsValidEntityId("npc:old_marta"));
    CHECK_FALSE(StateSchema::IsValidEntityId(""));
    CHECK_FALSE(StateSchema::IsValidEntityId("a.b"));
    CHECK_FALSE(StateSchema::IsValidEntityId("Player"));
}

TEST_CASE("StateSchema rejects integer bounds outside the int range", "[schema]")
{
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 3000000000LL}})),
                      ContainsSubstring("'bad'"));
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", -3000000000LL}, {"max", 0}})),
                      ContainsSubstring("'bad'"));
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField(
                          {{"type", "int"}, {"min", 0}, {"max", std::uint64_t{18446744073709551615ULL}}})),
                      ContainsSubstring("'bad'"));
    // Values that would wrap to a small valid int if converted without a range check.
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 4294967301LL}})),
                      ContainsSubstring("'bad'"));
    CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField(
                          {{"type", "int"}, {"min", 0}, {"max", std::uint64_t{9223372036854775813ULL}}})),
                      ContainsSubstring("'bad'"));
}

TEST_CASE("CheckFieldValue rejects out-of-range and non-integer ints", "[schema]")
{
    FieldDef health;
    health.type = FieldType::Int;
    health.min = 0;
    health.max = 10;
    CHECK_FALSE(StateSchema::CheckFieldValue(health, nlohmann::json(std::uint64_t{18446744073709551615ULL})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(health, 4.0).empty());

    // A uint64 above LLONG_MAX must not wrap to a negative value that happens to be in range.
    FieldDef signedField;
    signedField.type = FieldType::Int;
    signedField.min = -10;
    signedField.max = 10;
    CHECK_FALSE(StateSchema::CheckFieldValue(signedField, nlohmann::json(std::uint64_t{18446744073709551615ULL})).empty());
}

TEST_CASE("CheckFieldValue rejects bad counter shapes", "[schema]")
{
    FieldDef inventory;
    inventory.type = FieldType::Counter;
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"a.b", 1}})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"", 1}})).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, 5).empty());
}

TEST_CASE("CheckFieldValue caps counter counts at INT_MAX", "[schema]")
{
    FieldDef inventory;
    inventory.type = FieldType::Counter;
    CHECK_FALSE(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 2147483648LL}})).empty());
    CHECK_FALSE(
        StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", std::uint64_t{18446744073709551615ULL}}})).empty());
    CHECK(StateSchema::CheckFieldValue(inventory, nlohmann::json({{"rope", 2147483647}})).empty());
}

TEST_CASE("StateSchema accepts INT_MIN and INT_MAX as bounds", "[schema]")
{
    const StateSchema schema = StateSchema::FromJson(SchemaWithBadField(
        {{"type", "int"}, {"min", std::numeric_limits<int>::min()}, {"max", std::numeric_limits<int>::max()}}));

    const FieldDef& field = schema.FindEntityType("character")->fields.at("bad");
    CHECK(field.min == std::numeric_limits<int>::min());
    CHECK(field.max == std::numeric_limits<int>::max());
    CHECK(StateSchema::CheckFieldValue(field, std::numeric_limits<int>::min()).empty());
    CHECK(StateSchema::CheckFieldValue(field, std::numeric_limits<int>::max()).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(field, 2147483648LL).empty());
    CHECK_FALSE(StateSchema::CheckFieldValue(field, -2147483649LL).empty());
}

TEST_CASE("StateSchema rejects unknown keys and names them", "[schema]")
{
    SECTION("top level")
    {
        nlohmann::json json = SchemaJson();
        json["entity_typs"] = nlohmann::json::object();
        CHECK_THROWS_WITH(StateSchema::FromJson(json), ContainsSubstring("entity_typs"));
    }
    SECTION("entity type")
    {
        nlohmann::json json = SchemaJson();
        json["entity_types"]["npc"]["creatible"] = true;
        CHECK_THROWS_WITH(StateSchema::FromJson(json), ContainsSubstring("creatible"));
    }
    SECTION("field")
    {
        nlohmann::json json = SchemaJson();
        json["entity_types"]["npc"]["fields"]["health"]["defualt"] = 3;
        CHECK_THROWS_WITH(StateSchema::FromJson(json), ContainsSubstring("defualt"));
    }
    SECTION("min on a string field")
    {
        CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "string"}, {"min", 0}})),
                          ContainsSubstring("'min'"));
    }
    SECTION("values on an int field")
    {
        CHECK_THROWS_WITH(
            StateSchema::FromJson(SchemaWithBadField({{"type", "int"}, {"min", 0}, {"max", 3}, {"values", {"a"}}})),
            ContainsSubstring("'values'"));
    }
    SECTION("max on an enum field")
    {
        CHECK_THROWS_WITH(StateSchema::FromJson(SchemaWithBadField({{"type", "enum"}, {"values", {"a"}}, {"max", 3}})),
                          ContainsSubstring("'max'"));
    }
}
