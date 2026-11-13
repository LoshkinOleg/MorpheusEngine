# Schema-to-grammar library (sub-project B) - Design

Date: 2026-11-14. Source: requirement R3 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md` (the Skyrim/Mantella host), with the owner's decisions. Second of the integration's sub-projects A-G. A, the consumable build, is done.

## Goal

A reusable library that turns a bounded JSON shape into a GBNF grammar for llama.cpp, and checks a JSON value against the same shape:

- a public GBNF builder: the helpers that are private to `turn/ResolveGrammar.cpp` today, made general;
- `FromSchema(schema)` for a subset of JSON Schema;
- `Validate(value, schema)`, which reports mistakes as data;
- Morpheus's own resolve, lookup and review grammars rebuilt on the builder.

The Skyrim host needs this because its action list, and the actors an action may target, change on every exchange. Morpheus's own game keeps its behaviour.

## Owner decision

Approach B: the builder underneath, with `FromSchema` and `Validate` on top. They cover exactly R3's JSON Schema subset. Morpheus's three grammars move onto the builder, not onto JSON Schema, so the subset need not grow to express them.

## Module

- New library `morpheus_grammar` in `src/grammar/`, with namespace `morpheus::grammar` and alias `Morpheus::grammar`, defined like the other engine libraries (`morpheus_target_defaults`).
- It depends on `morpheus_core` (for `MORPHEUS_REQUIRE`, `ContractError`, `DataError` and `Utf8Length`) and nlohmann/json, and nothing else: no llama.cpp, no `inference`.
- `morpheus_turn` links it. In the dependency order it sits beside `context`: above `core`, below `turn`.
- The architecture spec's module table gains its row.

## 1. The builder (`grammar/Gbnf.h`)

Expressions are GBNF text fragments (`std::string`). The functions:

| Function | Matches |
|---|---|
| `Literal(text)` | `text` exactly (a GBNF string literal, `"` and `\` escaped) |
| `JsonString(minChars, maxChars)` | a JSON string of `minChars` to `maxChars` characters; each character is a code point or one JSON escape, exactly as the shared `char` terminal counts today. Requires `0 <= minChars <= maxChars <= 1999` |
| `IntRange(min, max)` | the decimal JSON integers from `min` to `max`, both included, exactly: no leading zeros, `-0` refused, any range of `long long` with `min <= max` |
| `Enum(values)` | one of the given strings, each as a JSON string. Requires at least one value and no duplicates |
| `ListOf(element, minItems, maxItems)` | a JSON array of `minItems` to `maxItems` elements, compact (no whitespace) as today. Requires `0 <= minItems <= maxItems`, `1 <= maxItems <= 1000`. `element` must be a rule reference or one string literal, nothing larger (name a larger element with `GrammarBuilder::Rule`) |
| `Object(fields)` | a JSON object with exactly the given keys, in the given order, compact. `fields` is a list of `{key, valueExpression}`; requires at least one field and no duplicate keys |
| `Alternatives(expressions)` | any one of them. Requires at least one |

A broken contract (for example `min > max`) is a `ContractError`.

`GrammarBuilder` collects named rules:

- `std::string Rule(std::string_view name, std::string body)` adds the rule `name ::= body` and returns the name, so it can be used in other expressions. A duplicate name is a `ContractError`. Names follow GBNF: `[a-z0-9-]+`.
- `std::string Text(std::string_view rootBody) const` returns the full grammar: the line `root ::= rootBody` first, then the rules in the order they were added, then the shared terminals the expressions used (`char` and the integer helper rules).

The root line comes first because `turn::CallKindOf` recognises a review and a lookup call by their grammar's first line, and that must not change.

- **Formatting:** the output stays compact JSON with no optional whitespace, like today's grammars, so the model's answer is one line.
- **Escaping:** `Literal` escapes for GBNF; values that sit inside JSON strings are JSON-escaped first, as `ResolveGrammar.cpp` does today for held item names.

## 2. Morpheus's grammars on the builder

`BuildResolveGrammar`, `BuildLookupGrammar` and `BuildReviewGrammar` (in `turn/ResolveGrammar.cpp`) are rewritten with the builder.

- **Same language:** each accepts exactly the set of strings it accepts today. That includes the counter path made of a fixed prefix plus a held item name or a new item matching `[a-z0-9_]{1,32}`, nonzero integers for `add` (the union of `IntRange(-999999999, -1)` and `IntRange(1, 999999999)`), and the review's tie between the verdict and its problems.
- **Exact integer ranges:** wherever today's grammars bound an integer by digit count only, the builder gives the same range: `int` is `IntRange(-999999999, 999999999)` and `pos-int` is `IntRange(1, 999999999)`.
- **First lines unchanged:** `root ::= review` for every review grammar, and the lookup grammar's current first line. `CallKindOf` and replays keep working.
- **Tests:**
  - tests that check what a grammar accepts or rejects, and the model-backed grammar tests, stay as they are and pass;
  - tests that compare exact grammar text are updated to the builder's text, and say so in their names or comments.
- **Removed:** the private helpers in `ResolveGrammar.cpp` (`Terminals`, `Literal`, `Alternatives`, `ListOf`), with no copies kept.

## 3. `FromSchema` (`grammar/Schema.h`)

`std::string FromSchema(const nlohmann::json& schema)` returns a complete grammar (root first) for the values the schema allows, built with the builder.

The supported subset is exactly this:

| Schema | Rules |
|---|---|
| `{"type": "object", "properties": {...}, "required": [...]}` | Every property must be in `required`; key order follows the `required` array (JSON objects carry no order). `additionalProperties` may be absent or `false`: extra keys are never allowed. `properties` may be empty only if `required` is empty, giving `{}` |
| `{"enum": [...]}` | all strings, or all integers |
| `{"const": v}` | a string or an integer |
| `{"type": "integer", "minimum": a, "maximum": b}` | both bounds required, `a <= b`, enforced exactly |
| `{"type": "string", "maxLength": n}` | `maxLength` required (at most 1999), optional `minLength`; lengths in characters |
| `{"type": "array", "items": {...}, "maxItems": n}` | `maxItems` required (1 to 1000), optional `minItems`. The element's grammar is a named rule `items-N`, so the list's repeated group stays within llama.cpp's limit |
| `{"oneOf": [...]}` | every branch an object schema. The discriminator is the first key of each branch's `required`. It must be the same key in every branch, and in each branch it must be a `const` string, different across branches |

- Keywords with no effect on values are allowed and ignored: `title`, `description`, `$comment`, `$schema`, `$id`.
- Any other keyword, a type outside the table, a missing required bound, or a broken rule (an empty `enum`, `minimum > maximum`, a non-object `oneOf` branch, a discriminator that is missing or repeated) is a `core::DataError`. Its message names the JSON pointer inside the schema (for example `/oneOf/3/properties/params/properties/target`) and the problem.
- Schemas are host data, so a bad one is a data error, not a contract error.
- **Two limits come from llama.cpp's grammar loader** (`llama-grammar.cpp`, `MAX_REPETITION_THRESHOLD` = 2000), which refuses a repetition count above 2000 and a repetition whose count times the rules it creates reaches 2000. A string repeats one rule reference, so `maxLength` is at most `GBNF_MAX_STRING_CHARS` = 1999; a list repeats a group of at most two symbols, so `maxItems` is at most `GBNF_MAX_LIST_ITEMS` = 1000. The builder throws `ContractError` above them; `FromSchema` and `Validate` throw `DataError` naming the keyword's pointer for a schema above them.
- Nested schemas follow the same rules at any depth. `$ref` is not supported.

## 4. `Validate` (`grammar/Schema.h`)

```cpp
struct ValidationError {
    std::string path = {};    // JSON pointer into the value, "" for the value itself.
    std::string message = {}; // What is wrong, naming the expected shape.
};
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);
```

- An empty result means the value is valid.
- It checks exactly what `FromSchema`'s grammar allows:
  - types (an integer is a JSON integer, not a float; integers are compared by their written form, so a huge unsigned never equals a negative);
  - `enum` and `const`;
  - integer ranges;
  - string lengths in characters (`core::Utf8Length`);
  - required keys present and no extra keys;
  - array sizes and each item.
- **`oneOf`:** the branch is chosen by the discriminator's value. A missing discriminator, or a value naming no branch, is one error at the discriminator's path. Otherwise the errors are those of the chosen branch, with full paths.
- It reports every error it finds, not only the first, in document order.
- **Never throws on the value:** a mistake in the value is data, as in `turn::ParseReviewOutput`. A schema outside the subset throws the same `DataError` as `FromSchema`.
- **Messages name the expected shape,** for example `'Saadia' is not one of: "0001A6A4", "00000007"`, `missing key 'target'`, `longer than 48 characters`, `unknown action 'dance'`.

## Tests (written first, Catch2)

**Builder:**
- the text of each function on small inputs;
- `IntRange` on hand-picked edge cases: single values, ranges across zero, negative-only ranges, ranges spanning several digit counts, the full `int` and `pos-int` ranges;
- the ContractErrors.

**Morpheus's grammars:** the existing tests, with exact-text comparisons updated as described in section 2.

**`FromSchema` and `Validate`:**
- The test schema is equivalent to the host's example catalog (section 5.2 of the requirements):
  - `oneOf` discriminated by `"action"`, with branches `none`, `follow`, `wait`, `attack`, `give_item` and `end_conversation`;
  - `attack`'s `params.target` is an `enum` of the present non-player actor ids;
  - `give_item`'s `params.item` is a string with `maxLength` 48;
  - the branches without parameters have empty `params` objects.
- Invalid values each give an error at the right path: an unknown action, an absent NPC as `target`, a missing `target`, an `item` over 48 characters, an extra key (the host's measured Qwen failure, `"intent"` added to `wait`), and a float where an integer is required.
- Each schema rule in section 3 has its own `DataError` test naming the pointer.

**Property test:**
- A test helper (`tests/grammar/RandomValue.h`) draws random valid values from a schema with a fixed seed. Strings include non-ASCII characters and JSON escapes, integers include the range ends, and arrays use every allowed size.
- In the fast suite, 1,000 values drawn from the catalog schema all pass `Validate`.
- In the model-backed suite (`[llama]`, `tests/llama/LlamaGrammarTests.cpp`):
  - `LlamaCppBackend::CheckGrammar(FromSchema(schema), value.dump())` accepts each of the same 1,000 values, written compact as the grammar expects;
  - each grammar `FromSchema` builds for the test schemas parses in llama.cpp;
  - a few hand-made invalid strings are rejected.

## Out of scope (later sub-projects)

- Turning the host's action catalog and the present actors into a schema, and filtering the allowed actions per turn: R5, sub-project D.
- Validating the host's observation object. Its schema uses floats, optional keys and booleans, which this subset does not cover: decided in sub-project D.
- An explicit call tag in `GenerationSpec`, so `CallKindOf` no longer reads grammar text: R9, sub-project G.

## Documentation

- The architecture spec gains the module row and a short paragraph on the library, the subset, and that Morpheus's grammars are built on it.
- The README's layout list gains `src/grammar`.
- CLAUDE.md's layout list gains `src/grammar`: "bounded-JSON grammars: the GBNF builder, `FromSchema` and `Validate`. No llama.cpp".
