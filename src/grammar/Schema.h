#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::grammar {

// A complete GBNF grammar (root line first), built with GrammarBuilder, for the compact JSON values `schema` allows.
// The schema is a subset of JSON Schema, exactly this:
//   {"type": "object", "properties": {...}, "required": [...]}  every property required; keys in `required` order;
//                                                               "additionalProperties" absent or false (extra keys
//                                                               are never allowed); empty properties and required
//                                                               give {}
//   {"enum": [...]}                                             all strings, or all integers
//   {"const": v}                                                a string or an integer
//   {"type": "integer", "minimum": a, "maximum": b}             both bounds, a <= b, exact
//   {"type": "string", "maxLength": n}                          optional "minLength"; lengths in characters
//   {"type": "array", "items": {...}, "maxItems": n}            n >= 1, optional "minItems"
//   {"oneOf": [...]}                                            every branch an object schema; the discriminator is
//                                                               the first key of each branch's `required`, the same
//                                                               key in every branch, a const string in each, and
//                                                               different across branches
// "title", "description", "$comment", "$schema" and "$id" are allowed anywhere and ignored. Nested schemas follow the
// same rules at any depth; "$ref" is not supported. So that llama.cpp loads the grammar, "maxLength" may be at most
// GBNF_MAX_STRING_CHARS and "maxItems" at most GBNF_MAX_LIST_ITEMS, and each array's items are a rule of their own
// (items-0, items-1, ...). Anything else is a core::DataError (schemas are host data) whose message names the JSON
// pointer inside the schema and the problem: "schema at <pointer>: <problem>".
std::string FromSchema(const nlohmann::json& schema);

} // namespace morpheus::grammar
