#pragma once

#include <nlohmann/json.hpp>

namespace morpheus::test {

// The host's example action catalog (integration requirements, section 5.2) as a schema of FromSchema's subset: one
// branch per action, told apart by "action". attack's target is one of the present non-player actors (Hulda
// 0001A6A4 and Saadia 0001B07C), give_item's item a string of at most 48 characters, and the actions without
// parameters have empty params objects. It carries ignored annotations, as a host's schema may.
inline nlohmann::json CatalogSchema()
{
    return nlohmann::json::parse(R"({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "action",
        "oneOf": [
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "none"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "follow"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "additionalProperties": false, "properties": {
                "action": {"const": "wait"},
                "params": {"type": "object", "properties": {}, "required": []}}},
            {"type": "object", "required": ["action", "params"], "description": "Attack a present actor.", "properties": {
                "action": {"const": "attack"},
                "params": {"type": "object", "required": ["target"], "properties": {
                    "target": {"enum": ["0001A6A4", "0001B07C"]}}}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "give_item"},
                "params": {"type": "object", "required": ["item"], "properties": {
                    "item": {"type": "string", "maxLength": 48}}}}},
            {"type": "object", "required": ["action", "params"], "properties": {
                "action": {"const": "end_conversation"},
                "params": {"type": "object", "properties": {}, "required": []}}}
        ]
    })");
}

// A schema that uses every keyword of the subset: integers (one over the whole long long range), a string with both
// lengths, arrays with and without minItems over string and integer enums, an integer const, an array of objects, an
// empty object and a nested oneOf. Its `required` orders are not alphabetical, so key order is tested too.
inline nlohmann::json EveryKeywordSchema()
{
    return nlohmann::json::parse(R"({
        "title": "Every keyword of the subset",
        "type": "object",
        "properties": {
            "count": {"type": "integer", "minimum": -20, "maximum": 120},
            "big": {"type": "integer", "minimum": -9223372036854775808, "maximum": 9223372036854775807},
            "name": {"type": "string", "minLength": 2, "maxLength": 6, "description": "Lengths count characters."},
            "tags": {"type": "array", "items": {"enum": ["red", "green", "blue"]}, "minItems": 1, "maxItems": 3},
            "codes": {"type": "array", "items": {"enum": [3, -7, 120]}, "maxItems": 2},
            "version": {"const": 2},
            "pair": {"type": "array", "minItems": 2, "maxItems": 2, "items": {
                "type": "object", "required": ["x", "label"], "properties": {
                    "x": {"type": "integer", "minimum": 0, "maximum": 9},
                    "label": {"const": "point"}}}},
            "empty": {"type": "object", "properties": {}, "required": [], "additionalProperties": false},
            "mode": {"oneOf": [
                {"type": "object", "required": ["state", "level"], "properties": {
                    "state": {"const": "on"},
                    "level": {"type": "integer", "minimum": 1, "maximum": 3}}},
                {"type": "object", "required": ["state"], "properties": {
                    "state": {"const": "off"}}}
            ]}
        },
        "required": ["name", "count", "big", "tags", "codes", "version", "pair", "empty", "mode"]
    })");
}

} // namespace morpheus::test
