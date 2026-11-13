# Schema-to-Grammar Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A reusable library, `morpheus_grammar`, that turns a bounded JSON shape into a GBNF grammar for llama.cpp (a public builder, and `FromSchema` for a subset of JSON Schema) and checks a JSON value against the same shape (`Validate`), with Morpheus's own resolve, lookup and review grammars rebuilt on the builder.

**Architecture:**
- **Library:** `src/grammar/`, namespace `morpheus::grammar`, alias `Morpheus::grammar`, depending on `morpheus_core` and nlohmann/json only. `morpheus_turn` links it. In the dependency order it sits beside `context`: above `core`, below `turn`.
- **Builder (`grammar/Gbnf.h`):** free functions `Literal`, `JsonString`, `IntRange`, `Enum`, `ListOf`, `Object`, `Alternatives` return GBNF text fragments; `GrammarBuilder` collects named rules (`Rule`) and writes the grammar (`Text`): the root line first, then the rules in the order added, then the shared terminal `char` if anything uses it.
- **Schema (`grammar/Schema.h`):** one internal step reads a schema into a checked tree (throwing `core::DataError` with a JSON pointer into the schema); `FromSchema` writes that tree with the builder, `Validate` walks it with the value and returns `ValidationError {path, message}`s.
- **Morpheus's grammars:** `turn/ResolveGrammar.cpp` is rewritten on the builder and its private helpers (`Terminals`, `Literal`, `Alternatives`, `ListOf`) are removed; every grammar accepts exactly the strings it accepts today, and its first line is unchanged.
- **Testing without llama.cpp:** a test-only GBNF matcher (`tests/grammar/GbnfMatcher.h`) lets the fast suite check what a grammar accepts: `IntRange` brute force, characterization of Morpheus's grammars before and after the rewrite, and the property test. The `[llama]` suite checks the same values with llama.cpp's own grammar sampler (`LlamaCppBackend::CheckGrammar`).
- **Spec:** `docs/superpowers/specs/2026-11-14-grammar-library-design.md` (the single source of requirements); requirement R3 of `.superpowers/briefs/2026-11-13-skyrim-mantella-integration-requirements.md`; architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`.

**Tech Stack:** C++20 with MSVC (VS 2022), CMake 3.25+, nlohmann/json 3.12.0, Catch2 v3, llama.cpp (dev preset, for the `[llama]` tests only). No new dependencies.

**Choices this plan makes where the spec is silent or cannot be followed literally (the owner may overrule any of them):**
- **A test-only GBNF matcher.** The fast suite has no GBNF matcher, and the `[llama]` suite cannot run there. The plan writes `tests/grammar/GbnfMatcher.h` (about 300 lines, header-only), which reads the GBNF subset the builder and today's grammars use (literals, classes, references, groups, `|`, `?`, `*`, `+`, `{m}`, `{m,}`, `{m,n}`) and matches code points as llama.cpp does. Reason: `IntRange`'s exactness needs tens of thousands of probes (every range between 26 hand-picked ends plus 200 random ranges, each probed over a window around both ends), which only a fast in-process matcher can run in every `tests` and `asan` build; the same matcher characterizes Morpheus's grammars before and after the rewrite (the "same language" requirement) and runs the property test's grammar check in the fast suite too. The `[llama]` suite then checks that llama.cpp agrees with the matcher on `IntRange` grammars, so the matcher is not the authority.
- **"Same language" is shown by characterization tests written before the rewrite.** Task 2 adds accept/reject tests, using the matcher, against today's grammars; they pass on today's code and must still pass after Task 5. They do not fail first: their purpose is to pin current behaviour.
- **Expressions with alternatives come in parentheses.** Every builder function returns an expression that can stand anywhere in a GBNF sequence, so `Enum`, `ListOf` with `minItems` 0, `IntRange` with more than one alternative and `Alternatives` with more than one expression are wrapped: `("[]" | "[" x ("," x){0,2} "]")`. Today's rule bodies `"[]" | ...` and `"\"wary\"" | "\"friendly\""` therefore gain parentheses; the exact-text tests that see them are updated (Task 5) and say so in a comment.
- **No integer helper rules.** The spec's `Text` appends "the shared terminals the expressions used (`char` and the integer helper rules)". `IntRange` writes its digit classes inline (it needs no shared rule), so `char` is the only shared terminal `Text` ever appends.
- **`GrammarBuilder::Text` checks references.** To find whether `char` is used it scans every body for rule names (outside literals, classes and repetition counts); every name found must be a rule added to the builder or `char`, or `Text` throws `ContractError`. `Rule` refuses the names `root` and `char` (reserved), names outside `[a-z0-9-]+`, a duplicate name, and a body that is empty or holds a line break; `Text` refuses an empty or multi-line root body.
- **`Literal` refuses a line break** (`ContractError`): a grammar is one rule per line, and `CallKindOf` reads the first line. Values that come from JSON are JSON-escaped first, so they never hold one.
- **`Object`'s field type is named `ObjectField {key, value}`** (the spec only says "a list of `{key, valueExpression}`").
- **How far Morpheus's grammars move onto the builder.** The roots, `check`, `lookup`, the review alternatives, every list (`ListOf`), every enum (`Enum`), the JSON strings (`JsonString`) and the integers (`IntRange`) use the builder's functions. The `change-N` bodies keep their literal sequences (built with `grammar::Literal`, `grammar::Enum` and `grammar::Alternatives`), because a counter change writes the item name inside the path string, which `Object` cannot express; their text is unchanged, so the tests that look for them do not change.
- **Rule order changes.** The builder appends rules in the order they are added, so the `enum-N`, `items-N` and `change-N` rules (written while the state is walked) now come before `checks`, `check` and the value rules. Order does not change a GBNF language.
- **`ResolveGrammar.h`:** the `static_assert` that every list bound is at least 2 is removed (the new `ListOf` needs `maxItems >= 1` and checks it), and `MAX_INT_MAGNITUDE = 999999999` (a `long long`) names the 9-digit bound of `int`, `nonzero-int` and `pos-int`.
- **`CallKindOf` needs no change.** It takes `BuildLookupGrammar()`'s first line at run time and checks `starts_with`; `Text` writes `root ::= "{\"lookups\":" lookups "}"` first, exactly today's line (`Object({{"lookups", "lookups"}})` gives today's text), and every review grammar still starts `root ::= review\n`. No resolve grammar's first line starts with the lookup's (it starts `root ::= "{\"intent\":"`).
- **Strict keyword sets.** Each row of the spec's table allows exactly its own keywords, plus the ignored ones. `type` together with `enum` or `const` (common in JSON Schema) is refused: `'type' and 'enum' cannot be combined`. An object needs both `properties` and `required`. A `oneOf` needs at least one branch, and a branch must be an object schema (`"type": "object"`). `enum` integers may be any JSON integer; `minimum` and `maximum` must fit a `long long`; lengths and item counts must be from 0 to 2147483647.
- **`DataError` message format:** `schema at <pointer>: <problem>`, with `the root` for the empty pointer. A mistake in a keyword's value names the keyword's own pointer (`/maxItems`, `/enum/2`, `/required/1`, `/oneOf/3`); a missing or inconsistent keyword names the schema's pointer. Pointer tokens are escaped as RFC 6901 says (`~` is `~0`, `/` is `~1`). Keywords outside the subset (`$ref`, `pattern`, `default`) are named first: `the keyword '$ref' is not supported`.
- **`Validate` details.** A missing key is reported at the key's own path (`/params/target`, like the discriminator). An unexpected key is reported at its own path (`/intent`). "Document order" is: an object's schema keys in `required` order (the order the grammar writes them), each with its nested errors, then the keys the schema does not allow, in nlohmann's key order. A `oneOf` value that is not an object is one error at its own path. The message formats are listed in Task 7.
- **Character counts.** `Validate` counts code points (`core::Utf8Length`); the grammar's `char` counts code points and escapes. They agree on compact text from `dump()`, which writes every character raw or as one escape. A model that writes an astral character as a surrogate-pair escape (`\ud83d\ude00`) spends two `char`s on one code point, so for a string with `minLength > 0` the grammar can accept a value that `Validate` reports as too short. This cannot be closed without changing `char`, which the spec keeps as it is.
- **U+007F.** The grammar's `char` excludes DEL (as today), while `dump()` writes DEL raw, so `RandomValue` never draws it (a value holding DEL is valid, but its `dump()` text is not in the grammar's language; its `\u007f` spelling is).
- **`RandomValue` returns `nlohmann::ordered_json`** with keys in `required` order, so `value.dump()` is the text the grammar expects; the property test validates `nlohmann::json::parse(value.dump())`, the host's own read of the model's text. It is a free function, `RandomValue(schema, random)`, over a `std::mt19937_64`; `PROPERTY_SEED` and `PROPERTY_DRAWS` (1000) are shared by the fast and `[llama]` suites so both check the same values.
- **Test schemas.** `tests/grammar/TestSchemas.h` holds `CatalogSchema()` (section 5.2's catalog: attack's present non-player actors are Hulda `0001A6A4` and Saadia `0001B07C`; `0001C3D2` stands for an absent NPC) and `EveryKeywordSchema()`, which uses every keyword (integers including the whole `long long` range, string `minLength`, array `minItems`, integer `enum` and `const`, a nested object array, an empty object, a nested `oneOf`) with `required` orders that are not alphabetical. The catalog has no integer, so the "float where an integer is required" test uses a one-field schema; the property test runs on both schemas (the spec asks for the catalog only).
- **Extra `[llama]` checks:** the three review grammars (which no `[llama]` test parsed before), and agreement between llama.cpp and the test matcher on four `IntRange` grammars.
- **The `[llama]` run** uses the configured `build/dev` tree, RelWithDebInfo (`build/dev/bin/RelWithDebInfo/morpheus_tests.exe`), as the model-seat plan did; the README's example uses the Debug binary, which works the same.
- **Documentation beyond the spec's list:** the README's and the architecture spec's lists of the engine libraries a host project gets gain `Morpheus::grammar`, the architecture spec's Testing section gains a `grammar` bullet, and `cmake/consumer-check/CMakeLists.txt`'s comment "the five engine libraries" becomes "six". Its program is not changed.

## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25. No new dependencies.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /permissive- /Zc:preprocessor /utf-8`, warnings as errors at top level): a warning fails the build.
- **Exact names (from the spec, verbatim):**
  - Library `morpheus_grammar` in `src/grammar/`, namespace `morpheus::grammar`, alias `Morpheus::grammar`, defined like the other engine libraries (`morpheus_target_defaults`).
  - `src/grammar/Gbnf.h`: `Literal(text)`, `JsonString(minChars, maxChars)`, `IntRange(min, max)`, `Enum(values)`, `ListOf(element, minItems, maxItems)`, `Object(fields)`, `Alternatives(expressions)`; class `GrammarBuilder` with `std::string Rule(std::string_view name, std::string body)` and `std::string Text(std::string_view rootBody) const`.
  - `src/grammar/Schema.h`: `std::string FromSchema(const nlohmann::json& schema)`; `struct ValidationError { std::string path = {}; std::string message = {}; };`; `std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)`.
  - Test helper `tests/grammar/RandomValue.h`.
- **Dependencies:** `morpheus_grammar` links `morpheus_core` and `nlohmann_json::nlohmann_json` only: no llama.cpp, no `inference`. `morpheus_turn` links it. Dependencies flow downward only.
- **Rules from the spec, verbatim:**
  - `IntRange`: "the decimal JSON integers from `min` to `max`, both included, exactly: no leading zeros, `-0` refused, any range of `long long` with `min <= max`".
  - `JsonString`: "each character is a code point or one JSON escape, exactly as the shared `char` terminal counts today. Requires `0 <= minChars <= maxChars`".
  - `ListOf`: "compact (no whitespace) as today. Requires `0 <= minItems <= maxItems` and `maxItems >= 1`".
  - `Object`: "exactly the given keys, in the given order, compact ... requires at least one field and no duplicate keys". `Enum`: "Requires at least one value and no duplicates". `Alternatives`: "Requires at least one".
  - "A broken contract (for example `min > max`) is a `ContractError`."
  - `Text`: "the line `root ::= rootBody` first, then the rules in the order they were added, then the shared terminals the expressions used".
  - Morpheus's grammars: "each accepts exactly the set of strings it accepts today"; "`int` is `IntRange(-999999999, 999999999)` and `pos-int` is `IntRange(1, 999999999)`"; `add` takes "the union of `IntRange(-999999999, -1)` and `IntRange(1, 999999999)`"; first lines unchanged: "`root ::= review` for every review grammar, and the lookup grammar's current first line".
  - `FromSchema`: the supported subset is exactly the spec's table; ignored keywords `title`, `description`, `$comment`, `$schema`, `$id`; "Any other keyword, a type outside the table, a missing required bound, or a broken rule ... is a `core::DataError`. Its message names the JSON pointer inside the schema"; "`$ref` is not supported".
  - `Validate`: "An empty result means the value is valid"; "It reports every error it finds, not only the first, in document order"; "Never throws on the value ... A schema outside the subset throws the same `DataError` as `FromSchema`".
- **Error policy:** engine bugs throw `core::ContractError` via `MORPHEUS_REQUIRE`. A bad schema is host data: `core::DataError` naming the JSON pointer. A mistake in a value is data (`ValidationError`), never an exception. No fallbacks that hide a broken contract.
- **Coding rules (`CLAUDE.md`):** types and methods PascalCase, locals and parameters camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; constants ALL_CAPS and grouped; members initialized at declaration; class layout: nested types, public data, private data, public methods, private methods, helpers; comments let a method read top to bottom; a private helper only with more than one caller (recursive helpers count their recursion; local lambdas are fine); no legacy paths, no copies of removed helpers. ASCII only in source comments and docs (non-ASCII test text is written as `\x` escapes).
- **Never name a local variable `grammar`** in a file that uses the `grammar::` namespace: the variable would hide the namespace in its own initializer.
- **Tests first:** every behavior lands with a Catch2 test written and run (failing) before the implementation; the characterization tests (Task 2) and the `[llama]` acceptance tests (Task 9) are the stated exceptions.
- **Shell:** commands run from `D:\MorpheusEngine` in Git Bash:
  - `cmake --build --preset tests`, `ctest --preset tests` (315 tests pass before this plan); one tag: `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`.
  - The `dev` tree is configured in `build/dev`: `cmake --build build/dev --config RelWithDebInfo --target <target>`.
  - New test files and new `CMakeLists.txt` entries are picked up by the next build (`CONFIGURE_DEPENDS`, and CMake re-runs itself when a `CMakeLists.txt` changes).
- **Never stage** `settings.json`, `models/`, `imgui.ini`, `docs/references/` or `.superpowers/`. Stage the files each commit step names, nothing else.
- **Commits** are made on `master`. Every commit message ends with a `Co-Authored-By:` line naming the implementing model. The steps show `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; an implementer running as Opus writes `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` instead.

## File Structure

```
tests/grammar/GbnfMatcher.h                 create: the test-only GBNF matcher (Task 1)
tests/grammar/GbnfMatcherTests.cpp          create: the matcher's own tests (Task 1)
tests/turn/ResolveGrammarTests.cpp          modify: characterization tests (Task 2), exact-text updates (Task 5)
src/grammar/CMakeLists.txt                  create: morpheus_grammar, Morpheus::grammar (Task 3; Schema.cpp in Task 6)
src/grammar/Gbnf.h, src/grammar/Gbnf.cpp    create: the builder (Task 3), IntRange (Task 4)
tests/grammar/GbnfTests.cpp                 create: builder tests (Task 3), IntRange tests (Task 4)
CMakeLists.txt                              modify: add_subdirectory(src/grammar) (Task 3)
tests/CMakeLists.txt                        modify: morpheus_tests links morpheus_grammar (Task 3)
src/turn/ResolveGrammar.h/.cpp              modify: rebuilt on the builder, helpers removed (Task 5)
src/turn/CMakeLists.txt                     modify: morpheus_turn links morpheus_grammar (Task 5)
src/grammar/Schema.h, src/grammar/Schema.cpp create: FromSchema (Task 6), Validate (Task 7)
tests/grammar/TestSchemas.h                 create: CatalogSchema, EveryKeywordSchema (Task 6)
tests/grammar/SchemaTests.cpp               create: FromSchema tests (Task 6), Validate tests (Task 7)
tests/grammar/RandomValue.h                 create: RandomValue, PROPERTY_SEED, PROPERTY_DRAWS (Task 8)
tests/grammar/PropertyTests.cpp             create: the fast-suite property tests (Task 8)
tests/llama/LlamaGrammarTests.cpp           modify: the model-backed grammar tests (Task 9)
docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md, README.md, CLAUDE.md,
cmake/consumer-check/CMakeLists.txt         modify: documentation (Task 10)
```

---

### Task 1: A test-only GBNF matcher

The fast suite cannot run llama.cpp, so it gets a small matcher for the GBNF the builder and today's grammars write. It parses one rule per line into a tree and matches by sets of end positions (every place an expression can end when it starts at a position), over the text's code points.

**Files:**
- Create: `tests/grammar/GbnfMatcher.h`
- Test: `tests/grammar/GbnfMatcherTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `class morpheus::test::GbnfMatcher` with `explicit GbnfMatcher(const std::string& grammar)` (throws `std::runtime_error` for a grammar it cannot read, a reference to an undefined rule, no `root`, a rule defined twice) and `bool Accepts(std::string_view text) const` (the root rule matches all of `text`; invalid UTF-8 never matches; a rule that comes back to itself without consuming text throws `std::runtime_error`).

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/GbnfMatcherTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "grammar/GbnfMatcher.h"

using namespace morpheus;

TEST_CASE("The test GBNF matcher reads literals, classes, groups and repetitions", "[gbnf-matcher]")
{
    const test::GbnfMatcher matcher(R"gbnf(root ::= "a" ("b" | "cd")? [x-z]{1,2} digits
digits ::= [0-9]*
)gbnf");
    CHECK(matcher.Accepts("ax"));
    CHECK(matcher.Accepts("abxy"));
    CHECK(matcher.Accepts("acdz123"));
    CHECK_FALSE(matcher.Accepts("a"));     // [x-z] at least once
    CHECK_FALSE(matcher.Accepts("axyz"));  // [x-z] at most twice
    CHECK_FALSE(matcher.Accepts("abcdx")); // "b" or "cd", not both
    CHECK_FALSE(matcher.Accepts("ax1a"));  // the root must match the whole text
    CHECK_FALSE(matcher.Accepts(""));

    const test::GbnfMatcher counts(R"gbnf(root ::= "<" [0-9]{2} ("," [0-9]){0,1} [a]+ [^a-z]{1,} ">"
)gbnf");
    CHECK(counts.Accepts("<12aA>"));
    CHECK(counts.Accepts("<12,3aaa!?>"));
    CHECK_FALSE(counts.Accepts("<1aA>"));      // {2} is exactly two
    CHECK_FALSE(counts.Accepts("<12,3,4aA>")); // {0,1} is at most one
    CHECK_FALSE(counts.Accepts("<12A>"));      // + needs one
    CHECK_FALSE(counts.Accepts("<12aa>"));     // {1,} needs one
}

TEST_CASE("The test GBNF matcher decodes escapes and UTF-8 as llama.cpp does", "[gbnf-matcher]")
{
    // The JSON string character of the grammar builder: a code point but ", \, DEL and the controls, or one escape.
    const test::GbnfMatcher matcher(R"gbnf(root ::= "\"" char{0,3} "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf");
    CHECK(matcher.Accepts(R"("")"));
    CHECK(matcher.Accepts(R"("abc")"));
    CHECK(matcher.Accepts("\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"")); // three characters of 2, 3 and 4 bytes
    CHECK(matcher.Accepts(R"("\n\"\u00e9")"));                          // three escapes
    CHECK_FALSE(matcher.Accepts(R"("abcd")"));                         // four characters
    CHECK_FALSE(matcher.Accepts("\"a\nb\""));                           // a raw control character
    CHECK_FALSE(matcher.Accepts("\"\x7F\""));                           // DEL
    CHECK_FALSE(matcher.Accepts(R"("\q")"));                           // not a JSON escape
    CHECK_FALSE(matcher.Accepts("\"\xC3\""));                           // not UTF-8

    // Literal escapes: \x, \u, \n and the quote.
    const test::GbnfMatcher escapes(R"gbnf(root ::= "\x41\u00e9\n\"" [\x30-\x32]
)gbnf");
    CHECK(escapes.Accepts("A\xC3\xA9\n\"1"));
    CHECK_FALSE(escapes.Accepts("A\xC3\xA9\n\"3"));
}

TEST_CASE("The test GBNF matcher refuses grammars it cannot read", "[gbnf-matcher]")
{
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= missing\n"), std::runtime_error);              // an undefined rule
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= (\"a\"\n"), std::runtime_error);               // an unclosed group
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\n"), std::runtime_error);                  // an unclosed literal
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= [a-z\n"), std::runtime_error);                 // an unclosed class
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\")\n"), std::runtime_error);               // an unmatched ')'
    CHECK_THROWS_AS(test::GbnfMatcher("other ::= \"a\"\n"), std::runtime_error);               // no root
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"a\"\nroot ::= \"b\"\n"), std::runtime_error); // defined twice
    CHECK_THROWS_AS(test::GbnfMatcher("root ::= \"\\q\"\n"), std::runtime_error);              // an unknown escape
    CHECK_THROWS_AS(test::GbnfMatcher("root \"a\"\n"), std::runtime_error);                     // no ' ::= '

    // A rule that comes back to itself without consuming text would loop: it is refused when matched.
    const test::GbnfMatcher recursive("root ::= a\na ::= root | \"x\"\n");
    CHECK_THROWS_AS(recursive.Accepts("x"), std::runtime_error);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/GbnfMatcher.h': No such file or directory` in `tests/grammar/GbnfMatcherTests.cpp`.

- [ ] **Step 3: Write the matcher**

Create `tests/grammar/GbnfMatcher.h`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morpheus::test {

// A test-only matcher for the GBNF that grammar::GrammarBuilder and Morpheus's grammars write, so the fast suite can
// check what a grammar accepts without llama.cpp (the [llama] tests check that llama.cpp agrees). It reads one rule
// per line, "name ::= body". A body has string literals ("..." with the escapes \" \\ \n \r \t \[ \] \xHH \uHHHH
// \UHHHHHHHH), character classes ([...] and [^...], with ranges and the same escapes), rule references, groups, "|"
// and the postfixes ?, *, +, {m}, {m,} and {m,n}. Like llama.cpp it matches the code points of the UTF-8 text, and the
// root rule must match all of it. Throws std::runtime_error for a grammar it cannot read, a reference to an undefined
// rule, or a rule that comes back to itself without consuming text.
class GbnfMatcher {
private:
    // One parsed expression.
    struct Node {
        enum class Kind { Literal, Class, Reference, Sequence, Choice, Repeat };

        Kind kind = Kind::Sequence;
        std::u32string text = {};                                // Literal: the code points to match.
        std::vector<std::pair<char32_t, char32_t>> ranges = {}; // Class: inclusive code point ranges.
        bool negated = false;                                    // Class: [^...].
        std::string rule = {};                                   // Reference: the rule's name.
        std::vector<Node> children = {};                         // Sequence, Choice: the parts; Repeat: the one item.
        std::size_t fewest = 0;                                  // Repeat: the least repetitions.
        std::size_t most = 0;                                    // Repeat: the most repetitions, or NO_LIMIT.
    };

    static constexpr std::size_t NO_LIMIT = static_cast<std::size_t>(-1);

    std::map<std::string, Node> _rules = {};

public:
    explicit GbnfMatcher(const std::string& grammar)
    {
        // One rule per line; blank lines are skipped.
        std::size_t start = 0;
        while (start < grammar.size()) {
            std::size_t end = grammar.find('\n', start);
            if (end == std::string::npos) {
                end = grammar.size();
            }
            const std::string line = grammar.substr(start, end - start);
            start = end + 1;
            if (line.empty()) {
                continue;
            }
            const std::size_t arrow = line.find(" ::= ");
            if (arrow == std::string::npos) {
                throw std::runtime_error("GbnfMatcher: a line without ' ::= ': " + line);
            }
            const std::string name = line.substr(0, arrow);
            std::size_t pos = arrow + 5;
            Node body = ParseChoice(line, pos);
            // ParseChoice stops at a ")" it did not open, or at the end of the line.
            if (pos != line.size()) {
                throw std::runtime_error("GbnfMatcher: an unmatched ')' in: " + line);
            }
            if (!_rules.emplace(name, std::move(body)).second) {
                throw std::runtime_error("GbnfMatcher: '" + name + "' is defined twice");
            }
        }
        if (!_rules.contains("root")) {
            throw std::runtime_error("GbnfMatcher: no root rule");
        }
        // Every reference must name a rule.
        for (const auto& [name, body] : _rules) {
            CheckReferences(body, name);
        }
    }

    // Whether the root rule matches all of `text`. Text that is not UTF-8 never matches.
    bool Accepts(std::string_view text) const
    {
        std::u32string codePoints;
        for (std::size_t pos = 0; pos < text.size();) {
            char32_t codePoint = 0;
            if (!DecodeUtf8(text, pos, codePoint)) {
                return false;
            }
            codePoints += codePoint;
        }
        std::set<std::pair<std::string, std::size_t>> active;
        return Match(_rules.at("root"), codePoints, 0, active).contains(codePoints.size());
    }

private:
    // Alternatives separated by "|", up to a ")" or the end of the line.
    Node ParseChoice(const std::string& line, std::size_t& pos) const
    {
        Node choice;
        choice.kind = Node::Kind::Choice;
        choice.children.push_back(ParseSequence(line, pos));
        while (pos < line.size() && line[pos] == '|') {
            ++pos;
            choice.children.push_back(ParseSequence(line, pos));
        }
        return choice;
    }

    // Items, each with its postfixes, up to a "|", a ")" or the end of the line.
    Node ParseSequence(const std::string& line, std::size_t& pos) const
    {
        Node sequence;
        sequence.kind = Node::Kind::Sequence;
        for (;;) {
            while (pos < line.size() && line[pos] == ' ') {
                ++pos;
            }
            if (pos >= line.size() || line[pos] == '|' || line[pos] == ')') {
                return sequence;
            }
            Node item = ParsePrimary(line, pos);
            // Each postfix wraps the item in a repetition: ? is {0,1}, * is {0,}, + is {1,}.
            while (pos < line.size() && (line[pos] == '?' || line[pos] == '*' || line[pos] == '+' || line[pos] == '{')) {
                Node repeat;
                repeat.kind = Node::Kind::Repeat;
                const char postfix = line[pos++];
                if (postfix == '?') {
                    repeat.fewest = 0;
                    repeat.most = 1;
                } else if (postfix == '*') {
                    repeat.fewest = 0;
                    repeat.most = NO_LIMIT;
                } else if (postfix == '+') {
                    repeat.fewest = 1;
                    repeat.most = NO_LIMIT;
                } else {
                    // {m}, {m,} or {m,n}.
                    const std::size_t close = line.find('}', pos);
                    if (close == std::string::npos) {
                        throw std::runtime_error("GbnfMatcher: an unclosed '{' in: " + line);
                    }
                    const std::string counts = line.substr(pos, close - pos);
                    pos = close + 1;
                    const std::size_t comma = counts.find(',');
                    repeat.fewest = static_cast<std::size_t>(std::stoull(counts.substr(0, comma)));
                    if (comma == std::string::npos) {
                        repeat.most = repeat.fewest;
                    } else if (comma + 1 == counts.size()) {
                        repeat.most = NO_LIMIT;
                    } else {
                        repeat.most = static_cast<std::size_t>(std::stoull(counts.substr(comma + 1)));
                    }
                }
                repeat.children.push_back(std::move(item));
                item = std::move(repeat);
            }
            sequence.children.push_back(std::move(item));
        }
    }

    // A string literal, a character class, a group or a rule reference.
    Node ParsePrimary(const std::string& line, std::size_t& pos) const
    {
        Node node;
        if (line[pos] == '"') {
            node.kind = Node::Kind::Literal;
            ++pos;
            while (pos < line.size() && line[pos] != '"') {
                node.text += ParseChar(line, pos);
            }
            if (pos >= line.size()) {
                throw std::runtime_error("GbnfMatcher: an unclosed string literal in: " + line);
            }
            ++pos;
        } else if (line[pos] == '[') {
            node.kind = Node::Kind::Class;
            ++pos;
            if (pos < line.size() && line[pos] == '^') {
                node.negated = true;
                ++pos;
            }
            while (pos < line.size() && line[pos] != ']') {
                // One character, or a range "a-b" (a "-" just before "]" is a plain character).
                const char32_t low = ParseChar(line, pos);
                char32_t high = low;
                if (pos + 1 < line.size() && line[pos] == '-' && line[pos + 1] != ']') {
                    ++pos;
                    high = ParseChar(line, pos);
                }
                node.ranges.emplace_back(low, high);
            }
            if (pos >= line.size()) {
                throw std::runtime_error("GbnfMatcher: an unclosed character class in: " + line);
            }
            ++pos;
        } else if (line[pos] == '(') {
            ++pos;
            node = ParseChoice(line, pos);
            if (pos >= line.size() || line[pos] != ')') {
                throw std::runtime_error("GbnfMatcher: an unclosed '(' in: " + line);
            }
            ++pos;
        } else if (IsNameChar(line[pos])) {
            node.kind = Node::Kind::Reference;
            while (pos < line.size() && IsNameChar(line[pos])) {
                node.rule += line[pos++];
            }
        } else {
            throw std::runtime_error(std::string("GbnfMatcher: an unexpected '") + line[pos] + "' in: " + line);
        }
        return node;
    }

    // One character of a literal or a class: an escape, or one UTF-8 code point.
    char32_t ParseChar(const std::string& line, std::size_t& pos) const
    {
        if (line[pos] != '\\') {
            char32_t codePoint = 0;
            if (!DecodeUtf8(line, pos, codePoint)) {
                throw std::runtime_error("GbnfMatcher: invalid UTF-8 in: " + line);
            }
            return codePoint;
        }
        if (pos + 1 >= line.size()) {
            throw std::runtime_error("GbnfMatcher: an escape at the end of: " + line);
        }
        const char escape = line[pos + 1];
        pos += 2;
        // The single-character escapes, or the number of hex digits that follow.
        std::size_t digits = 0;
        switch (escape) {
        case 'n':
            return U'\n';
        case 'r':
            return U'\r';
        case 't':
            return U'\t';
        case '\\':
        case '"':
        case '[':
        case ']':
            return static_cast<char32_t>(escape);
        case 'x':
            digits = 2;
            break;
        case 'u':
            digits = 4;
            break;
        case 'U':
            digits = 8;
            break;
        default:
            throw std::runtime_error(std::string("GbnfMatcher: an unknown escape '\\") + escape + "' in: " + line);
        }
        if (pos + digits > line.size()) {
            throw std::runtime_error("GbnfMatcher: a short hex escape in: " + line);
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const char c = line[pos + i];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                throw std::runtime_error("GbnfMatcher: a bad hex digit in: " + line);
            }
            value = value * 16 + digit;
        }
        pos += digits;
        return static_cast<char32_t>(value);
    }

    // Throws unless every reference inside `node` (part of the rule `name`) names a rule. Recursive over the tree.
    void CheckReferences(const Node& node, const std::string& name) const
    {
        if (node.kind == Node::Kind::Reference && !_rules.contains(node.rule)) {
            throw std::runtime_error("GbnfMatcher: '" + node.rule + "' used by '" + name + "' is not defined");
        }
        for (const Node& child : node.children) {
            CheckReferences(child, name);
        }
    }

    // The positions where `node` can end when it starts at `pos` of `text`. `active` holds the rule references being
    // matched with their positions, so a rule that comes back to itself at the same position throws instead of
    // looping. Recursive over the tree.
    std::set<std::size_t> Match(const Node& node, const std::u32string& text, std::size_t pos,
                                std::set<std::pair<std::string, std::size_t>>& active) const
    {
        std::set<std::size_t> ends;
        switch (node.kind) {
        case Node::Kind::Literal:
            // compare() takes what is left when the text is shorter, which then differs from the literal.
            if (text.compare(pos, node.text.size(), node.text) == 0) {
                ends.insert(pos + node.text.size());
            }
            break;
        case Node::Kind::Class:
            if (pos < text.size()) {
                const char32_t c = text[pos];
                const bool inRanges = std::any_of(node.ranges.begin(), node.ranges.end(),
                                                  [c](const auto& range) { return range.first <= c && c <= range.second; });
                if (inRanges != node.negated) {
                    ends.insert(pos + 1);
                }
            }
            break;
        case Node::Kind::Reference: {
            const std::pair<std::string, std::size_t> call(node.rule, pos);
            if (!active.insert(call).second) {
                throw std::runtime_error("GbnfMatcher: '" + node.rule + "' comes back to itself without consuming text");
            }
            ends = Match(_rules.at(node.rule), text, pos, active);
            active.erase(call);
            break;
        }
        case Node::Kind::Sequence: {
            // Each part starts wherever the parts before it can end.
            ends.insert(pos);
            for (const Node& child : node.children) {
                std::set<std::size_t> next;
                for (const std::size_t start : ends) {
                    const std::set<std::size_t> more = Match(child, text, start, active);
                    next.insert(more.begin(), more.end());
                }
                ends = std::move(next);
                if (ends.empty()) {
                    break;
                }
            }
            break;
        }
        case Node::Kind::Choice:
            for (const Node& child : node.children) {
                const std::set<std::size_t> more = Match(child, text, pos, active);
                ends.insert(more.begin(), more.end());
            }
            break;
        case Node::Kind::Repeat: {
            // Without a limit, more repetitions than characters left beyond `fewest` cannot reach a new position.
            const std::size_t most = node.most == NO_LIMIT ? node.fewest + (text.size() - pos) : node.most;
            std::set<std::size_t> current = {pos};
            if (node.fewest == 0) {
                ends.insert(pos);
            }
            for (std::size_t count = 1; count <= most && !current.empty(); ++count) {
                std::set<std::size_t> next;
                for (const std::size_t start : current) {
                    const std::set<std::size_t> more = Match(node.children[0], text, start, active);
                    next.insert(more.begin(), more.end());
                }
                current = std::move(next);
                if (count >= node.fewest) {
                    ends.insert(current.begin(), current.end());
                }
            }
            break;
        }
        }
        return ends;
    }

    static bool IsNameChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    }

    // Decodes the UTF-8 code point at `pos` and moves past it; false for bytes that are not UTF-8.
    static bool DecodeUtf8(std::string_view text, std::size_t& pos, char32_t& codePoint)
    {
        auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
        const unsigned char lead = byte(pos);
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (lead < 0x80) {
            length = 1;
            value = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            value = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            value = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            value = lead & 0x07u;
        } else {
            return false;
        }
        if (pos + length > text.size()) {
            return false;
        }
        // Each continuation byte is 10xxxxxx and adds six bits.
        for (std::size_t i = 1; i < length; ++i) {
            if ((byte(pos + i) & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte(pos + i) & 0x3Fu);
        }
        pos += length;
        codePoint = static_cast<char32_t>(value);
        return true;
    }
};

} // namespace morpheus::test
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[gbnf-matcher]"`
Expected: builds with no warnings; `All tests passed` (3 test cases).

- [ ] **Step 5: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 318`.

- [ ] **Step 6: Commit**

```bash
git add tests/grammar/GbnfMatcher.h tests/grammar/GbnfMatcherTests.cpp
git commit -m "Add a test-only GBNF matcher for the fast suite" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Characterize what Morpheus's grammars accept

Before the rewrite, pin the language of today's resolve, lookup and review grammars with accept/reject tests on the matcher. They pass on today's code (that is their point) and must pass unchanged after Task 5. If one fails here, the matcher or the expectation is wrong: fix the matcher or the test, never `ResolveGrammar.cpp`, and say so in the task report.

**Files:**
- Modify: `tests/turn/ResolveGrammarTests.cpp`

**Interfaces:**
- Consumes: `test::GbnfMatcher` (Task 1); `turn::BuildResolveGrammar`, `turn::BuildLookupGrammar`, `turn::BuildReviewGrammar` and the size constants in `turn/ResolveGrammar.h` (unchanged).
- Produces: six `[grammar]` test cases that Task 5 must keep green.

- [ ] **Step 1: Add the include and a list helper**

In `tests/turn/ResolveGrammarTests.cpp`, replace

```cpp
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
```

with

```cpp
#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <vector>
```

replace

```cpp
#include "TestPaths.h"
```

with

```cpp
#include "TestPaths.h"
#include "grammar/GbnfMatcher.h"
```

and replace

```cpp
std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}
```

with

```cpp
std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// A compact JSON array of `count` copies of the JSON text `item`.
std::string JsonList(const std::string& item, int count)
{
    std::string items;
    for (int i = 0; i < count; ++i) {
        items += (i == 0 ? "" : ",") + item;
    }
    return "[" + items + "]";
}
```

- [ ] **Step 2: Write the characterization tests**

Append to `tests/turn/ResolveGrammarTests.cpp`:

```cpp
// Characterization: what the grammars accept and refuse, checked with the test matcher. The builder rewrite keeps
// every one of these, so they show the rewritten grammars accept the same language.

TEST_CASE("The resolve grammar accepts the proposals it describes and refuses the rest", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher engine(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));

    const std::vector<std::string> accepted = {
        R"({"intent":"look","checks":[],"changes":[]})",
        R"({"intent":"climb","checks":[{"stat":"agility","difficulty":"hard","stakes":"a fall","on_success":[{"op":"set","path":"player.location","value":"cliff top","reason":"climbs up"}],"on_failure":[{"op":"add","path":"player.health","value":-2,"reason":"falls"}]}],"changes":[]})",
        R"({"intent":"light","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":-1,"reason":"burns oil"},{"op":"remove","path":"player.inventory.lantern","reason":"set in place"},{"op":"add","path":"player.inventory.lantern","value":999999999,"reason":"r"}]})",
        R"({"intent":"meet","checks":[],"changes":[{"op":"create","path":"npc:marta","value":{"type":"npc","fields":{"disposition":"wary"}},"reason":"she arrives"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":999999999,"reason":"r"},{"op":"set","path":"player.agility","value":-999999999,"reason":"r"},{"op":"add","path":"player.health","value":0,"reason":"r"}]})",
        R"({"intent":"caf\u00e9 \"quoted\"\n","checks":[],"changes":[]})",
        "{\"intent\":\"caf\xC3\xA9 \xE6\x97\xA5\",\"checks\":[],\"changes\":[]}",
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(engine.Accepts(text));
    }

    const std::vector<std::string> rejected = {
        // Integers: no "-0", no leading zero, at most 9 digits; a counter add is never zero.
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":-0,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":05,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":1000000000,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.health","value":-1000000000,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":0,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.oil","value":-0,"reason":"r"}]})",
        // The world's stats and difficulties only.
        R"({"intent":"x","checks":[{"stat":"strength","difficulty":"hard","stakes":"s","on_success":[],"on_failure":[]}],"changes":[]})",
        R"({"intent":"x","checks":[{"stat":"agility","difficulty":"medium","stakes":"s","on_success":[],"on_failure":[]}],"changes":[]})",
        // Compact, in order, and notes only in assisted mode.
        R"({"intent": "look","checks":[],"changes":[]})",
        R"({"intent":"look","changes":[],"checks":[]})",
        R"({"intent":"look","checks":[],"changes":[],"notes":[]})",
        // Only the shapes the schema allows.
        R"({"intent":"x","checks":[],"changes":[{"op":"remove","path":"player.inventory.lantern","value":1,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"npc:marta.disposition","value":"wary","reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.inventory.Oil","value":1,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.location","value":5,"reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"add","path":"player.location","value":"x","reason":"r"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"set","path":"player.location","value":"x"}]})",
        R"({"intent":"x","checks":[],"changes":[{"op":"create","path":"npc:marta","value":{"type":"npc","fields":{"disposition":"angry"}},"reason":"r"}]})",
        // Text: no raw control character, no raw DEL.
        "{\"intent\":\"a\nb\",\"checks\":[],\"changes\":[]}",
        "{\"intent\":\"a\x7F\",\"checks\":[],\"changes\":[]}",
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(engine.Accepts(text));
    }
}

TEST_CASE("The resolve grammar bounds its lists and texts exactly", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher engine(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Engine));
    const std::string change = R"({"op":"set","path":"player.location","value":"x","reason":"r"})";
    // A check whose success branch holds `branchChanges` changes.
    auto checkWith = [&](int branchChanges) {
        return R"({"stat":"agility","difficulty":"easy","stakes":"s","on_success":)" + JsonList(change, branchChanges) +
               R"(,"on_failure":[]})";
    };
    auto proposal = [&](const std::string& intent, int checks, int branchChanges, int changes) {
        return R"({"intent":")" + intent + R"(","checks":)" + JsonList(checkWith(branchChanges), checks) +
               R"(,"changes":)" + JsonList(change, changes) + "}";
    };
    const std::size_t shortText = static_cast<std::size_t>(turn::SHORT_TEXT_CHARS);

    CHECK(engine.Accepts(proposal(std::string(shortText, 'a'), turn::MAX_CHECKS, turn::MAX_BRANCH_CHANGES,
                                  turn::MAX_CHANGES)));
    CHECK_FALSE(engine.Accepts(proposal(std::string(shortText + 1, 'a'), 0, 0, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", turn::MAX_CHECKS + 1, 0, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", 1, turn::MAX_BRANCH_CHANGES + 1, 0)));
    CHECK_FALSE(engine.Accepts(proposal("a", 0, 0, turn::MAX_CHANGES + 1)));
}

TEST_CASE("The assisted resolve grammar and the lookup grammar accept exactly their shapes", "[grammar]")
{
    const core::World world = MiniWorld();
    const test::GbnfMatcher assisted(turn::BuildResolveGrammar(world, world.initialState, core::MemoryMode::Assisted));
    const std::string note(static_cast<std::size_t>(turn::LONG_TEXT_CHARS), 'n');
    CHECK(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":[]})"));
    CHECK(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":["The keeper hums.",")" + note + R"("]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":["a","b","c"]})"));
    CHECK_FALSE(assisted.Accepts(R"({"intent":"look","checks":[],"changes":[],"notes":[")" + note + R"(n"]})"));

    const test::GbnfMatcher lookup(turn::BuildLookupGrammar());
    CHECK(lookup.Accepts(R"({"lookups":[]})"));
    CHECK(lookup.Accepts(R"({"lookups":[{"source":"lore","query":"keeper"},{"source":"history","query":""},{"source":"notes","query":"the shanty"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"lore","query":"a"},{"source":"lore","query":"b"},{"source":"lore","query":"c"},{"source":"lore","query":"d"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"web","query":"x"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"query":"x","source":"lore"}]})"));
    CHECK_FALSE(lookup.Accepts(R"({"lookups":[{"source":"lore"}]})"));
}

TEST_CASE("The resolve grammar's creates accept exactly the field values of their type", "[grammar]")
{
    const nlohmann::json schema = nlohmann::json::parse(R"({
        "entity_types": {
            "pack": {
                "creatable": true,
                "fields": {
                    "contents": {"type": "counter"},
                    "kind": {"type": "enum", "values": ["small", "large"]},
                    "label": {"type": "string"},
                    "size": {"type": "int", "min": 0, "max": 9},
                    "tags": {"type": "list"}
                }
            }
        }
    })");
    const nlohmann::json state = nlohmann::json::object();
    const core::World world = WorldWith(schema, state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    // A create of a pack with the id `id` and the fields object `fields`.
    auto create = [](const std::string& id, const std::string& fields) {
        return R"({"intent":"pack","checks":[],"changes":[{"op":"create","path":")" + id +
               R"(","value":{"type":"pack","fields":)" + fields + R"(},"reason":"r"}]})";
    };
    // A counter value with `count` new items, i0 to i<count - 1>, of one each.
    auto contents = [](int count) {
        std::string items;
        for (int i = 0; i < count; ++i) {
            items += std::format(R"({}"i{}":1)", i == 0 ? "" : ",", i);
        }
        return "{" + items + "}";
    };
    const std::string rest = R"("kind":"small","label":"bag","size":0,"tags":[])";
    const std::size_t idChars = static_cast<std::size_t>(turn::MAX_NEW_ID_CHARS);

    CHECK(matcher.Accepts(create("pack:1", R"({"contents":{},)" + rest + "}")));
    CHECK(matcher.Accepts(create("pack_2", R"({"contents":{"rope":2,"oil":999999999},"kind":"large","label":"","size":-3,"tags":["a","b"]})")));
    CHECK(matcher.Accepts(create(std::string(idChars, 'p'), R"({"contents":{},)" + rest + "}")));
    CHECK(matcher.Accepts(create("pack:1", R"({"contents":)" + contents(turn::MAX_LIST_ITEMS) + "," + rest + "}")));
    CHECK(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0,"tags":)" +
                                               JsonList(R"("t")", turn::MAX_LIST_ITEMS) + "}")));

    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":)" + contents(turn::MAX_LIST_ITEMS + 1) + "," + rest + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0,"tags":)" +
                                                     JsonList(R"("t")", turn::MAX_LIST_ITEMS + 1) + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{"rope":0},)" + rest + "}"))); // counts are positive
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{"Rope":1},)" + rest + "}"))); // new items: [a-z0-9_]
    CHECK_FALSE(matcher.Accepts(create("Pack:1", R"({"contents":{},)" + rest + "}")));         // ids: [a-z0-9_:]
    CHECK_FALSE(matcher.Accepts(create(std::string(idChars + 1, 'p'), R"({"contents":{},)" + rest + "}")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"small","label":"bag","size":0})")));
    CHECK_FALSE(matcher.Accepts(create("pack:1", R"({"contents":{},"kind":"medium","label":"bag","size":0,"tags":[]})")));
}

TEST_CASE("The resolve grammar accepts held counter items by name and list values", "[grammar]")
{
    const nlohmann::json state = PlayerState({{"Rusty Knife", 1}, {"say \"hi\"", 1}});
    const core::World world = WorldWith(AllFieldsSchema(), state);
    const test::GbnfMatcher matcher(turn::BuildResolveGrammar(world, state, core::MemoryMode::Engine));
    auto proposal = [](const std::string& change) {
        return R"({"intent":"x","checks":[],"changes":[)" + change + "]}";
    };
    CHECK(matcher.Accepts(proposal(R"({"op":"remove","path":"player.inventory.Rusty Knife","reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"add","path":"player.inventory.say \"hi\"","value":2,"reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"add","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK(matcher.Accepts(proposal(R"({"op":"remove","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"remove","path":"player.inventory.Rusty Spoon","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"set","path":"player.conditions","value":"wet","reason":"r"})")));
    CHECK_FALSE(matcher.Accepts(proposal(R"({"op":"add","path":"player.inventory.say "hi"","value":2,"reason":"r"})")));
}

TEST_CASE("The review grammars accept exactly the verdicts and digests asked for", "[grammar]")
{
    const std::string digest300(static_cast<std::size_t>(turn::DIGEST_MAX_CHARS), 'd');
    const std::string problem200(static_cast<std::size_t>(turn::REVIEW_PROBLEM_MAX_CHARS), 'p');

    const test::GbnfMatcher both(turn::BuildReviewGrammar(true, true));
    CHECK(both.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));
    CHECK(both.Accepts(R"({"consistent":false,"problems":["p","q","r"],"digest":""})"));
    CHECK(both.Accepts(R"({"consistent":true,"problems":[],"digest":")" + digest300 + R"("})"));
    CHECK(both.Accepts(R"({"consistent":false,"problems":[")" + problem200 + R"("],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":["p"],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":["p","q","r","s"],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[""],"digest":"d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":[]})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":true,"problems":[],"digest":")" + digest300 + R"(d"})"));
    CHECK_FALSE(both.Accepts(R"({"consistent":false,"problems":[")" + problem200 + R"(p"],"digest":"d"})"));

    const test::GbnfMatcher check(turn::BuildReviewGrammar(true, false));
    CHECK(check.Accepts(R"({"consistent":true,"problems":[]})"));
    CHECK(check.Accepts(R"({"consistent":false,"problems":["p"]})"));
    CHECK_FALSE(check.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));

    const test::GbnfMatcher digest(turn::BuildReviewGrammar(false, true));
    CHECK(digest.Accepts(R"({"digest":"d"})"));
    CHECK_FALSE(digest.Accepts(R"({"consistent":true,"problems":[],"digest":"d"})"));
}
```

- [ ] **Step 3: Run the tests: they pass on today's grammars**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: builds with no warnings; `All tests passed` (18 test cases: the twelve existing `[grammar]` cases and these six). A failure here is a bug in the matcher or in the expectation (see the task intro), not in the grammar.

- [ ] **Step 4: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 324`.

- [ ] **Step 5: Commit**

```bash
git add tests/turn/ResolveGrammarTests.cpp
git commit -m "Characterize what the resolve, lookup and review grammars accept" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The `morpheus_grammar` library and the builder

The library and every builder function except `IntRange` (Task 4). Each function returns an expression that can stand in a GBNF sequence; `GrammarBuilder` writes the root line first.

**Files:**
- Create: `src/grammar/CMakeLists.txt`, `src/grammar/Gbnf.h`, `src/grammar/Gbnf.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/grammar/GbnfTests.cpp`

**Interfaces:**
- Consumes: `MORPHEUS_REQUIRE` and `core::ContractError` (`core/Errors.h`); `test::GbnfMatcher` (Task 1).
- Produces (namespace `morpheus::grammar`, header `grammar/Gbnf.h`):
  - `struct ObjectField { std::string key = {}; std::string value = {}; };`
  - `std::string Literal(std::string_view text);`
  - `std::string JsonString(int minChars, int maxChars);`
  - `std::string Enum(const std::vector<std::string>& values);`
  - `std::string ListOf(std::string_view element, int minItems, int maxItems);`
  - `std::string Object(const std::vector<ObjectField>& fields);`
  - `std::string Alternatives(const std::vector<std::string>& expressions);`
  - `class GrammarBuilder` with `std::string Rule(std::string_view name, std::string body);` and `std::string Text(std::string_view rootBody) const;`
  - CMake target `morpheus_grammar`, alias `Morpheus::grammar`; `morpheus_tests` links it.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/GbnfTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "core/Errors.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"

using namespace morpheus;

namespace {

// The shared `char` terminal, as GrammarBuilder::Text appends it.
const std::string CHAR_LINE =
    R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))gbnf" "\n";

// A matcher for the grammar whose root is `rootBody`.
test::GbnfMatcher MatcherFor(const std::string& rootBody)
{
    return test::GbnfMatcher(grammar::GrammarBuilder().Text(rootBody));
}

} // namespace

TEST_CASE("Literal matches its text exactly, with quotes and backslashes escaped", "[gbnf]")
{
    CHECK(grammar::Literal("abc") == R"("abc")");
    CHECK(grammar::Literal(R"(a"b\c)") == R"("a\"b\\c")");
    CHECK(grammar::Literal("") == R"("")");
    CHECK(grammar::Literal("caf\xC3\xA9") == "\"caf\xC3\xA9\"");
    // A grammar is one rule per line, so a literal cannot hold a line break.
    CHECK_THROWS_AS(grammar::Literal("a\nb"), core::ContractError);
    CHECK_THROWS_AS(grammar::Literal("a\rb"), core::ContractError);
}

TEST_CASE("JsonString bounds a JSON string by characters", "[gbnf]")
{
    CHECK(grammar::JsonString(0, 120) == R"gbnf("\"" char{0,120} "\"")gbnf");
    CHECK(grammar::JsonString(1, 200) == R"gbnf("\"" char{1,200} "\"")gbnf");
    CHECK(grammar::JsonString(3, 3) == R"gbnf("\"" char{3} "\"")gbnf");
    CHECK(grammar::JsonString(0, 0) == R"gbnf("\"\"")gbnf");
    CHECK_THROWS_AS(grammar::JsonString(-1, 2), core::ContractError);
    CHECK_THROWS_AS(grammar::JsonString(3, 2), core::ContractError);

    // Each character is a code point or one escape; raw control characters, DEL and lone quotes are refused.
    const test::GbnfMatcher text = MatcherFor(grammar::JsonString(1, 3));
    CHECK(text.Accepts(R"("a")"));
    CHECK(text.Accepts(R"("\u00e9\n\"")"));
    CHECK(text.Accepts("\"\xE6\x97\xA5\xF0\x9F\x98\x80\xC3\xA9\""));
    CHECK_FALSE(text.Accepts(R"("")"));
    CHECK_FALSE(text.Accepts(R"("abcd")"));
    CHECK_FALSE(text.Accepts("\"a\tb\""));
    CHECK_FALSE(text.Accepts("\"\x7F\""));
    CHECK_FALSE(text.Accepts(R"("a"")"));
    CHECK_FALSE(text.Accepts("a"));
}

TEST_CASE("Enum matches one of its values as a JSON string", "[gbnf]")
{
    CHECK(grammar::Enum({"wary"}) == R"gbnf("\"wary\"")gbnf");
    CHECK(grammar::Enum({"wary", "friendly"}) == R"gbnf(("\"wary\"" | "\"friendly\""))gbnf");
    // The value is JSON-escaped, then escaped once more as a GBNF literal.
    CHECK(grammar::Enum({R"(a"b\c)"}) == R"gbnf("\"a\\\"b\\\\c\"")gbnf");
    CHECK_THROWS_AS(grammar::Enum({}), core::ContractError);
    CHECK_THROWS_AS(grammar::Enum({"a", "b", "a"}), core::ContractError);

    const test::GbnfMatcher mood = MatcherFor(grammar::Enum({"wary", R"(a"b)"}));
    CHECK(mood.Accepts(R"("wary")"));
    CHECK(mood.Accepts(R"("a\"b")"));
    CHECK_FALSE(mood.Accepts("wary"));
    CHECK_FALSE(mood.Accepts(R"("friendly")"));
}

TEST_CASE("ListOf bounds a compact JSON array", "[gbnf]")
{
    CHECK(grammar::ListOf("x", 0, 3) == R"gbnf(("[]" | "[" x ("," x){0,2} "]"))gbnf");
    CHECK(grammar::ListOf("x", 1, 3) == R"gbnf("[" x ("," x){0,2} "]")gbnf");
    CHECK(grammar::ListOf("x", 2, 5) == R"gbnf("[" x ("," x){1,4} "]")gbnf");
    CHECK(grammar::ListOf("x", 2, 2) == R"gbnf("[" x ("," x){1} "]")gbnf");
    CHECK(grammar::ListOf("x", 0, 1) == R"gbnf(("[]" | "[" x "]"))gbnf");
    CHECK(grammar::ListOf("x", 1, 1) == R"gbnf("[" x "]")gbnf");
    CHECK_THROWS_AS(grammar::ListOf("x", 0, 0), core::ContractError);
    CHECK_THROWS_AS(grammar::ListOf("x", -1, 2), core::ContractError);
    CHECK_THROWS_AS(grammar::ListOf("x", 3, 2), core::ContractError);

    const test::GbnfMatcher list = MatcherFor(grammar::ListOf(grammar::Literal("7"), 0, 2));
    CHECK(list.Accepts("[]"));
    CHECK(list.Accepts("[7]"));
    CHECK(list.Accepts("[7,7]"));
    CHECK_FALSE(list.Accepts("[7,7,7]"));
    CHECK_FALSE(list.Accepts("[ 7]"));
    CHECK_FALSE(list.Accepts("[7,]"));
    CHECK_FALSE(list.Accepts("[,]"));
}

TEST_CASE("Object matches a compact JSON object with fixed keys in a fixed order", "[gbnf]")
{
    CHECK(grammar::Object({{"a", "x"}}) == R"gbnf("{\"a\":" x "}")gbnf");
    CHECK(grammar::Object({{"a", "x"}, {"b", "y"}}) == R"gbnf("{\"a\":" x ",\"b\":" y "}")gbnf");
    CHECK(grammar::Object({{"say \"hi\"", "x"}}) == R"gbnf("{\"say \\\"hi\\\"\":" x "}")gbnf");
    CHECK_THROWS_AS(grammar::Object({}), core::ContractError);
    CHECK_THROWS_AS(grammar::Object({{"a", "x"}, {"a", "y"}}), core::ContractError);

    const test::GbnfMatcher object =
        MatcherFor(grammar::Object({{"a", grammar::Enum({"x", "y"})}, {"b", grammar::Literal("1")}}));
    CHECK(object.Accepts(R"({"a":"x","b":1})"));
    CHECK(object.Accepts(R"({"a":"y","b":1})"));
    CHECK_FALSE(object.Accepts(R"({"b":1,"a":"x"})"));
    CHECK_FALSE(object.Accepts(R"({"a":"x"})"));
    CHECK_FALSE(object.Accepts(R"({"a":"x","b":1,"c":1})"));
    CHECK_FALSE(object.Accepts(R"({"a": "x","b":1})"));
    CHECK_FALSE(object.Accepts(R"({"a":"z","b":1})"));
}

TEST_CASE("Alternatives matches any one of its expressions and can stand in a sequence", "[gbnf]")
{
    CHECK(grammar::Alternatives({"a"}) == "a");
    CHECK(grammar::Alternatives({"a", "b c"}) == "(a | b c)");
    CHECK_THROWS_AS(grammar::Alternatives({}), core::ContractError);

    const test::GbnfMatcher either = MatcherFor(grammar::Literal("<") + " " +
                                                grammar::Alternatives({grammar::Literal("yes"), grammar::Literal("no")}) +
                                                " " + grammar::Literal(">"));
    CHECK(either.Accepts("<yes>"));
    CHECK(either.Accepts("<no>"));
    CHECK_FALSE(either.Accepts("<yes"));
    CHECK_FALSE(either.Accepts("no>"));
    CHECK_FALSE(either.Accepts("<yesno>"));
}

TEST_CASE("GrammarBuilder writes the root line first, then its rules, then the shared terminals used", "[gbnf]")
{
    grammar::GrammarBuilder builder;
    CHECK(builder.Rule("item-list", grammar::ListOf("item", 0, 2)) == "item-list");
    CHECK(builder.Rule("item", grammar::JsonString(1, 4)) == "item");
    CHECK(builder.Text(grammar::Object({{"items", "item-list"}})) == R"gbnf(root ::= "{\"items\":" item-list "}"
item-list ::= ("[]" | "[" item ("," item){0,1} "]")
item ::= "\"" char{1,4} "\""
)gbnf" + CHAR_LINE);

    // Without a JSON string there is no `char` rule.
    CHECK(grammar::GrammarBuilder().Text(grammar::Literal("yes")) == "root ::= \"yes\"\n");
}

TEST_CASE("GrammarBuilder refuses broken rules and undefined references", "[gbnf]")
{
    grammar::GrammarBuilder builder;
    builder.Rule("a", grammar::Literal("a"));
    CHECK_THROWS_AS(builder.Rule("a", grammar::Literal("b")), core::ContractError);          // defined twice
    CHECK_THROWS_AS(builder.Rule("Upper", grammar::Literal("b")), core::ContractError);      // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("snake_case", grammar::Literal("b")), core::ContractError); // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("two words", grammar::Literal("b")), core::ContractError);  // not [a-z0-9-]
    CHECK_THROWS_AS(builder.Rule("", grammar::Literal("b")), core::ContractError);
    CHECK_THROWS_AS(builder.Rule("root", grammar::Literal("b")), core::ContractError);       // reserved
    CHECK_THROWS_AS(builder.Rule("char", grammar::Literal("b")), core::ContractError);       // reserved
    CHECK_THROWS_AS(builder.Rule("b", ""), core::ContractError);                             // an empty body
    CHECK_THROWS_AS(builder.Rule("b", "a\na"), core::ContractError);                         // two lines
    CHECK_THROWS_AS(builder.Text(""), core::ContractError);
    CHECK_THROWS_AS(builder.Text("a\na"), core::ContractError);

    // Every name the root and the rules use must be defined.
    CHECK(builder.Text("a") == "root ::= a\na ::= \"a\"\n");
    CHECK_THROWS_AS(builder.Text("a missing"), core::ContractError);
    builder.Rule("b", "a undefined-rule");
    CHECK_THROWS_AS(builder.Text("a"), core::ContractError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Gbnf.h': No such file or directory` in `tests/grammar/GbnfTests.cpp`.

- [ ] **Step 3: Define the library in CMake**

Create `src/grammar/CMakeLists.txt`:

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
)
add_library(Morpheus::grammar ALIAS morpheus_grammar) # The name host projects link.
target_include_directories(morpheus_grammar PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_grammar PUBLIC morpheus_core nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_grammar)
```

In `CMakeLists.txt`, replace

```cmake
add_subdirectory(src/context)
add_subdirectory(src/turn)
```

with

```cmake
add_subdirectory(src/context)
add_subdirectory(src/grammar)
add_subdirectory(src/turn)
```

In `tests/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_turn morpheus_app_model morpheus_app_ui imgui_core Catch2::Catch2WithMain)
```

with

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_grammar morpheus_turn morpheus_app_model morpheus_app_ui imgui_core Catch2::Catch2WithMain)
```

- [ ] **Step 4: Write the header**

Create `src/grammar/Gbnf.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morpheus::grammar {

// GBNF text fragments for llama.cpp grammars of compact JSON: no optional whitespace, so the model's answer is one
// line. Every function returns an expression that can stand anywhere in a GBNF sequence: one made of alternatives
// comes in parentheses. A broken contract (for example min > max) is a core::ContractError.

// One key of an Object and the expression for its value.
struct ObjectField {
    std::string key = {};   // The key; Object writes it as a JSON string.
    std::string value = {}; // A GBNF expression for the value.
};

// `text` exactly: a GBNF string literal, with `"` and `\` escaped. A grammar is one rule per line, so `text` must not
// hold a line break. Values that sit inside JSON strings are JSON-escaped by the caller first.
std::string Literal(std::string_view text);

// A JSON string of minChars to maxChars characters. Each character is a code point other than `"`, `\`, U+007F and
// the control characters, or one JSON escape: the shared terminal `char`. Requires 0 <= minChars <= maxChars.
std::string JsonString(int minChars, int maxChars);

// One of `values`, each written as a JSON string. Requires at least one value and no duplicates.
std::string Enum(const std::vector<std::string>& values);

// A JSON array of minItems to maxItems `element`s, compact. Requires 0 <= minItems <= maxItems and maxItems >= 1.
std::string ListOf(std::string_view element, int minItems, int maxItems);

// A JSON object with exactly the keys of `fields`, in their order, compact. Requires at least one field and no
// duplicate keys.
std::string Object(const std::vector<ObjectField>& fields);

// Any one of `expressions`. Requires at least one.
std::string Alternatives(const std::vector<std::string>& expressions);

// Collects named rules into a whole grammar.
class GrammarBuilder {
private:
    std::vector<std::pair<std::string, std::string>> _rules = {}; // Name and body, in the order they were added.

public:
    // Adds the rule `name ::= body` and returns `name`, for use in other expressions. Names follow GBNF: [a-z0-9-]+.
    // "root" and the shared terminal's name ("char") are reserved. A duplicate name, or a body that is empty or holds
    // a line break, is a ContractError.
    std::string Rule(std::string_view name, std::string body);

    // The full grammar: the line `root ::= rootBody` first, then the rules in the order they were added, then the
    // shared terminals the root and the rules use (`char`). The root line comes first because turn::CallKindOf
    // recognises a review and a lookup call by their grammar's first line. Every rule name the root and the rules use
    // must be one added here or a shared terminal; an undefined one, or a root body that is empty or holds a line
    // break, is a ContractError.
    std::string Text(std::string_view rootBody) const;
};

} // namespace morpheus::grammar
```

- [ ] **Step 5: Write the implementation**

Create `src/grammar/Gbnf.cpp`:

```cpp
#include "grammar/Gbnf.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>

#include <nlohmann/json.hpp>

#include "core/Errors.h"

namespace morpheus::grammar {

namespace {

// The shared terminal `char`: one character of a JSON string, a code point other than `"`, `\`, DEL and the control
// characters, or one escape. Text appends it when the root or a rule uses it.
constexpr std::string_view CHAR_RULE_NAME = "char";
constexpr std::string_view CHAR_RULE =
    R"gbnf(char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))gbnf";

// A repetition count: "{n}" for exactly n, "{m,n}" for a range.
std::string Count(std::size_t fewest, std::size_t most)
{
    return fewest == most ? std::format("{{{}}}", most) : std::format("{{{},{}}}", fewest, most);
}

} // namespace

std::string Literal(std::string_view text)
{
    MORPHEUS_REQUIRE(text.find_first_of("\r\n") == std::string_view::npos,
                     "Literal: the text must not hold a line break");
    // Inside a GBNF string literal only the quote and the backslash need a backslash.
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result + "\"";
}

std::string JsonString(int minChars, int maxChars)
{
    MORPHEUS_REQUIRE(0 <= minChars && minChars <= maxChars, "JsonString: needs 0 <= minChars <= maxChars");
    // No characters at all: just the two quotes.
    if (maxChars == 0) {
        return Literal("\"\"");
    }
    return Literal("\"") + " char" + Count(static_cast<std::size_t>(minChars), static_cast<std::size_t>(maxChars)) +
           " " + Literal("\"");
}

std::string Enum(const std::vector<std::string>& values)
{
    MORPHEUS_REQUIRE(!values.empty(), "Enum: needs at least one value");
    MORPHEUS_REQUIRE(std::set<std::string>(values.begin(), values.end()).size() == values.size(),
                     "Enum: the values must be distinct");
    // Each value is a JSON string inside the answer, so its JSON text is what the literal matches.
    std::vector<std::string> literals;
    for (const std::string& value : values) {
        literals.push_back(Literal(nlohmann::json(value).dump()));
    }
    return Alternatives(literals);
}

std::string ListOf(std::string_view element, int minItems, int maxItems)
{
    MORPHEUS_REQUIRE(0 <= minItems && minItems <= maxItems && maxItems >= 1,
                     "ListOf: needs 0 <= minItems <= maxItems and maxItems >= 1");
    // The first element is written out; a repetition of "," and an element adds the others.
    const std::size_t fewestMore = static_cast<std::size_t>(std::max(minItems, 1) - 1);
    const std::size_t mostMore = static_cast<std::size_t>(maxItems - 1);
    std::string items = Literal("[") + " " + std::string(element);
    if (mostMore > 0) {
        items += " (" + Literal(",") + " " + std::string(element) + ")" + Count(fewestMore, mostMore);
    }
    items += " " + Literal("]");
    // An empty list is an alternative of its own.
    return minItems == 0 ? Alternatives({Literal("[]"), items}) : items;
}

std::string Object(const std::vector<ObjectField>& fields)
{
    MORPHEUS_REQUIRE(!fields.empty(), "Object: needs at least one field");
    std::set<std::string> keys;
    std::string result;
    for (const ObjectField& field : fields) {
        MORPHEUS_REQUIRE(keys.insert(field.key).second, "Object: the key '" + field.key + "' is given twice");
        // The opening brace or the comma before it, the key as a JSON string and its colon: one literal.
        result += Literal((result.empty() ? "{" : ",") + nlohmann::json(field.key).dump() + ":") + " " + field.value + " ";
    }
    return result + Literal("}");
}

std::string Alternatives(const std::vector<std::string>& expressions)
{
    MORPHEUS_REQUIRE(!expressions.empty(), "Alternatives: needs at least one expression");
    // One expression stands alone; several are grouped, so that the result can stand in a sequence.
    if (expressions.size() == 1) {
        return expressions[0];
    }
    std::string result = "(";
    for (std::size_t i = 0; i < expressions.size(); ++i) {
        result += (i == 0 ? "" : " | ") + expressions[i];
    }
    return result + ")";
}

std::string GrammarBuilder::Rule(std::string_view name, std::string body)
{
    const bool wellFormed = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
    MORPHEUS_REQUIRE(wellFormed, "GrammarBuilder::Rule: '" + std::string(name) + "' is not a GBNF rule name ([a-z0-9-]+)");
    MORPHEUS_REQUIRE(name != "root" && name != CHAR_RULE_NAME,
                     "GrammarBuilder::Rule: '" + std::string(name) + "' is reserved");
    const bool taken = std::any_of(_rules.begin(), _rules.end(), [&](const auto& rule) { return rule.first == name; });
    MORPHEUS_REQUIRE(!taken, "GrammarBuilder::Rule: '" + std::string(name) + "' is defined twice");
    MORPHEUS_REQUIRE(!body.empty() && body.find_first_of("\r\n") == std::string::npos,
                     "GrammarBuilder::Rule: the body of '" + std::string(name) + "' must be one non-empty line");
    _rules.emplace_back(std::string(name), std::move(body));
    return std::string(name);
}

std::string GrammarBuilder::Text(std::string_view rootBody) const
{
    MORPHEUS_REQUIRE(!rootBody.empty() && rootBody.find_first_of("\r\n") == std::string_view::npos,
                     "GrammarBuilder::Text: the root body must be one non-empty line");
    // Every line of the grammar, the root first.
    std::vector<std::pair<std::string, std::string>> lines = {{"root", std::string(rootBody)}};
    lines.insert(lines.end(), _rules.begin(), _rules.end());

    // The rule names the bodies use: the words outside string literals, character classes and repetition counts.
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    std::set<std::string> used;
    for (const auto& [name, body] : lines) {
        std::size_t i = 0;
        while (i < body.size()) {
            const char c = body[i];
            if (c == '"' || c == '[') {
                // A literal or a class: skip to its closer, past backslash escapes.
                const char closer = c == '"' ? '"' : ']';
                std::size_t j = i + 1;
                while (j < body.size() && body[j] != closer) {
                    j += body[j] == '\\' ? 2 : 1;
                }
                MORPHEUS_REQUIRE(j < body.size(), "GrammarBuilder::Text: an unclosed literal or class in '" + name + "'");
                i = j + 1;
            } else if (c == '{') {
                // A repetition count such as {0,5}.
                const std::size_t close = body.find('}', i);
                MORPHEUS_REQUIRE(close != std::string::npos,
                                 "GrammarBuilder::Text: an unclosed repetition in '" + name + "'");
                i = close + 1;
            } else if (isNameChar(c)) {
                std::size_t end = i;
                while (end < body.size() && isNameChar(body[end])) {
                    ++end;
                }
                used.insert(body.substr(i, end - i));
                i = end;
            } else {
                ++i;
            }
        }
    }

    // Each name used must be a rule added here or a shared terminal.
    for (const std::string& reference : used) {
        const bool defined = reference == CHAR_RULE_NAME ||
                             std::any_of(_rules.begin(), _rules.end(),
                                         [&](const auto& rule) { return rule.first == reference; });
        MORPHEUS_REQUIRE(defined, "GrammarBuilder::Text: '" + reference + "' is used but not defined");
    }

    // The root line, the rules in order, then the shared terminals used.
    std::string text;
    for (const auto& [name, body] : lines) {
        text += name + " ::= " + body + "\n";
    }
    if (used.contains(std::string(CHAR_RULE_NAME))) {
        text += std::string(CHAR_RULE) + "\n";
    }
    return text;
}

} // namespace morpheus::grammar
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[gbnf]"`
Expected: builds with no warnings; `All tests passed` (8 test cases).

- [ ] **Step 7: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 332`.

- [ ] **Step 8: Commit**

```bash
git add src/grammar/CMakeLists.txt src/grammar/Gbnf.h src/grammar/Gbnf.cpp CMakeLists.txt tests/CMakeLists.txt tests/grammar/GbnfTests.cpp
git commit -m "Add morpheus_grammar with the GBNF builder" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `IntRange`, exact for any `long long` range

**The algorithm.** `IntRange(min, max)` splits the range at zero and writes each side as alternatives:
1. **Sides.** The negative numbers are `"-"` followed by a magnitude from `|min(max, -1)|` to `|min|`; zero is the literal `"0"`; the positive numbers are magnitudes from `max(min, 1)` to `max`. Magnitudes are `unsigned long long`, computed as `0ULL - static_cast<unsigned long long>(v)`, so the magnitude of `LLONG_MIN` (2^63) fits. Since every magnitude is at least 1 and written without a leading zero, no `-0` and no leading zeros can appear.
2. **Bands by digit count.** A magnitude range `lo..hi` (1 <= lo <= hi) is split by digit count `d`: the band `10^(d-1)..10^d - 1`. The lowest band starts at `lo` and is partial unless `lo` is a power of ten; the highest ends at `hi` and is partial unless `hi` is all nines; the bands in between are full. Consecutive full bands share one alternative: `[1-9]` followed by `[0-9]{a,b}` free digits (so `1..999999999` is `[1-9] [0-9]{0,8}`, today's `pos-int`).
3. **One band, digit by digit.** A partial band is a range of digit strings of one length, `low..high`. Past their shared prefix (written once as a literal), the first differing digit `a < b` splits it into: `a` followed by the range `low's rest..99..9`; the free middle `[a+1-b-1]` followed by free digits; and `b` followed by `00..0..high's rest`. When `low`'s rest is all zeros, `a` joins the free middle; when `high`'s rest is all nines, `b` does. The two ends recurse on strings one digit shorter, so the depth is at most 19.

Each alternative is a sequence; a group with more than one alternative is wrapped in parentheses. Exactness follows from the decomposition: the bands partition `lo..hi` by length, and within a band the three parts partition it by the first differing digit. Tests: hand-computed texts (single values, across zero, negative only, several digit counts, partial bottom and top bands, Morpheus's `int` and `pos-int`), a brute force over many ranges with the matcher (every integer in a window around both ends is accepted iff it is in range, and no other spelling is ever accepted), and the whole `long long` range at its edges.

**Files:**
- Modify: `src/grammar/Gbnf.h`, `src/grammar/Gbnf.cpp`
- Test: `tests/grammar/GbnfTests.cpp`

**Interfaces:**
- Consumes: Task 3's builder; `test::GbnfMatcher`.
- Produces: `std::string grammar::IntRange(long long min, long long max);` (`ContractError` when `min > max`).

- [ ] **Step 1: Write the failing tests**

In `tests/grammar/GbnfTests.cpp`, replace

```cpp
#include <string>
#include <vector>
```

with

```cpp
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
```

and append:

```cpp
TEST_CASE("IntRange writes each digit count and boundary as alternatives", "[int-range]")
{
    // Morpheus's own ranges: int, pos-int and the negative half of nonzero-int.
    CHECK(grammar::IntRange(-999999999, 999999999) == R"gbnf(("-" [1-9] [0-9]{0,8} | "0" | [1-9] [0-9]{0,8}))gbnf");
    CHECK(grammar::IntRange(1, 999999999) == "[1-9] [0-9]{0,8}");
    CHECK(grammar::IntRange(-999999999, -1) == R"gbnf("-" [1-9] [0-9]{0,8})gbnf");
    // Single values.
    CHECK(grammar::IntRange(0, 0) == R"gbnf("0")gbnf");
    CHECK(grammar::IntRange(7, 7) == R"gbnf("7")gbnf");
    CHECK(grammar::IntRange(-7, -7) == R"gbnf("-" "7")gbnf");
    CHECK(grammar::IntRange(250, 250) == R"gbnf("250")gbnf");
    // Across zero, and within one digit count.
    CHECK(grammar::IntRange(-1, 1) == R"gbnf(("-" "1" | "0" | "1"))gbnf");
    CHECK(grammar::IntRange(0, 9) == R"gbnf(("0" | [1-9]))gbnf");
    CHECK(grammar::IntRange(10, 99) == "[1-9] [0-9]");
    // Several digit counts: a partial top band, a negative side, a partial bottom band.
    CHECK(grammar::IntRange(0, 255) ==
          R"gbnf(("0" | [1-9] [0-9]{0,1} | "1" [0-9]{2} | "2" ([0-4] [0-9] | "5" [0-5])))gbnf");
    CHECK(grammar::IntRange(-15, 123) ==
          R"gbnf(("-" ([1-9] | "1" [0-5]) | "0" | [1-9] [0-9]{0,1} | "1" ([0-1] [0-9] | "2" [0-3])))gbnf");
    CHECK(grammar::IntRange(37, 120) == R"gbnf(("3" [7-9] | [4-9] [0-9] | "1" ([0-1] [0-9] | "2" "0")))gbnf");

    CHECK_THROWS_AS(grammar::IntRange(2, 1), core::ContractError);
}

TEST_CASE("IntRange accepts exactly the integers of its range, each written one way", "[int-range]")
{
    // Every range between two of these ends, and random ranges, each probed around both ends and inside.
    const std::vector<long long> ends = {-1001, -1000, -999, -101, -100, -99, -91, -90, -10, -9, -1, 0, 1,
                                         9,     10,    11,   19,   20,   99,  100, 101, 199, 255, 999, 1000, 1001};
    std::vector<std::pair<long long, long long>> ranges;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        for (std::size_t j = i; j < ends.size(); ++j) {
            ranges.emplace_back(ends[i], ends[j]);
        }
    }
    std::mt19937_64 random(20261114);
    for (int i = 0; i < 200; ++i) {
        const long long low = std::uniform_int_distribution<long long>(-1000000, 1000000)(random);
        ranges.emplace_back(low, low + std::uniform_int_distribution<long long>(0, 100000)(random));
    }

    for (const auto& [low, high] : ranges) {
        const test::GbnfMatcher matcher = MatcherFor(grammar::IntRange(low, high));
        std::vector<long long> probes;
        for (long long delta = -15; delta <= 15; ++delta) {
            probes.push_back(low + delta);
            probes.push_back(high + delta);
        }
        for (long long eighth = 1; eighth < 8; ++eighth) {
            probes.push_back(low + (high - low) * eighth / 8);
        }
        for (const long long value : probes) {
            INFO("IntRange(" << low << ", " << high << ") on " << value);
            CHECK(matcher.Accepts(std::to_string(value)) == (low <= value && value <= high));
        }
        // Whatever the range, no other spelling: a leading zero, "-0", a plus sign, a space, nothing.
        for (const long long value : {low, high}) {
            const std::string digits = std::to_string(value < 0 ? -value : value);
            INFO("IntRange(" << low << ", " << high << ") on other spellings of " << value);
            CHECK_FALSE(matcher.Accepts((value < 0 ? "-0" : "0") + digits));
            CHECK_FALSE(matcher.Accepts("+" + digits));
            CHECK_FALSE(matcher.Accepts(" " + std::to_string(value)));
        }
        CHECK_FALSE(matcher.Accepts("-0"));
        CHECK_FALSE(matcher.Accepts("-"));
        CHECK_FALSE(matcher.Accepts(""));
    }
}

TEST_CASE("IntRange covers the whole long long range exactly", "[int-range]")
{
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();

    const test::GbnfMatcher all = MatcherFor(grammar::IntRange(LOWEST, HIGHEST));
    CHECK(all.Accepts("-9223372036854775808"));
    CHECK(all.Accepts("9223372036854775807"));
    CHECK(all.Accepts("0"));
    CHECK(all.Accepts("-1"));
    CHECK(all.Accepts("1000000000000000000"));
    CHECK(all.Accepts("-999999999999999999"));
    CHECK_FALSE(all.Accepts("-9223372036854775809"));
    CHECK_FALSE(all.Accepts("9223372036854775808"));
    CHECK_FALSE(all.Accepts("9999999999999999999"));
    CHECK_FALSE(all.Accepts("10000000000000000000"));
    CHECK_FALSE(all.Accepts("-0"));
    CHECK_FALSE(all.Accepts("01"));

    // A window at each end.
    const test::GbnfMatcher top = MatcherFor(grammar::IntRange(HIGHEST - 30, HIGHEST));
    for (long long value = HIGHEST - 40; value < HIGHEST; ++value) {
        INFO(value);
        CHECK(top.Accepts(std::to_string(value)) == (value >= HIGHEST - 30));
    }
    CHECK(top.Accepts("9223372036854775807"));
    CHECK_FALSE(top.Accepts("9223372036854775808"));
    const test::GbnfMatcher bottom = MatcherFor(grammar::IntRange(LOWEST, LOWEST + 30));
    for (long long value = LOWEST + 1; value < LOWEST + 40; ++value) {
        INFO(value);
        CHECK(bottom.Accepts(std::to_string(value)) == (value <= LOWEST + 30));
    }
    CHECK(bottom.Accepts("-9223372036854775808"));
    CHECK_FALSE(bottom.Accepts("-9223372036854775809"));

    // The lowest value alone.
    const test::GbnfMatcher lowest = MatcherFor(grammar::IntRange(LOWEST, LOWEST));
    CHECK(lowest.Accepts("-9223372036854775808"));
    CHECK_FALSE(lowest.Accepts("-9223372036854775807"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/grammar/GbnfTests.cpp`: `'IntRange': is not a member of 'morpheus::grammar'`.

- [ ] **Step 3: Declare `IntRange`**

In `src/grammar/Gbnf.h`, replace

```cpp
std::string JsonString(int minChars, int maxChars);
```

with

```cpp
std::string JsonString(int minChars, int maxChars);

// The decimal JSON integers from min to max, both included, exactly: no leading zeros, no "-0", no "+". Any range of
// long long with min <= max. Its digit classes are written inline: it needs no shared terminal.
std::string IntRange(long long min, long long max);
```

- [ ] **Step 4: Implement it**

In `src/grammar/Gbnf.cpp`, replace

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
```

with

```cpp
#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
#include <string>
```

replace

```cpp
// A repetition count: "{n}" for exactly n, "{m,n}" for a range.
std::string Count(std::size_t fewest, std::size_t most)
{
    return fewest == most ? std::format("{{{}}}", most) : std::format("{{{},{}}}", fewest, most);
}

} // namespace
```

with

```cpp
// A repetition count: "{n}" for exactly n, "{m,n}" for a range.
std::string Count(std::size_t fewest, std::size_t most)
{
    return fewest == most ? std::format("{{{}}}", most) : std::format("{{{},{}}}", fewest, most);
}

// fewest to most free digits.
std::string AnyDigits(std::size_t fewest, std::size_t most)
{
    return fewest == 1 && most == 1 ? std::string("[0-9]") : "[0-9]" + Count(fewest, most);
}

// The alternatives matching every digit string as long as `low`, from `low` to `high` (equal lengths, low <= high,
// which as digit strings of one length compares as numbers). Leading zeros are allowed here: the callers give a first
// digit of 1 to 9, or a suffix. Recursive: past the shared prefix, the first digit that differs splits the range into
// its low end, a middle of free digits and its high end.
std::vector<std::string> SameLengthAlternatives(const std::string& low, const std::string& high)
{
    if (low == high) {
        return {Literal(low)};
    }
    // A digit from `from` to `to`: a literal for one digit, a class for several.
    auto digitsFrom = [](char from, char to) {
        return from == to ? Literal(std::string(1, from)) : std::format("[{}-{}]", from, to);
    };

    // The first position where the two differ; the digits before it are shared.
    std::size_t split = 0;
    while (low[split] == high[split]) {
        ++split;
    }
    const char a = low[split];
    const char b = high[split];
    const std::size_t rest = low.size() - split - 1; // Digits after the split.
    std::vector<std::string> alternatives;
    if (rest == 0) {
        alternatives.push_back(digitsFrom(a, b));
    } else {
        const std::string lowRest = low.substr(split + 1);
        const std::string highRest = high.substr(split + 1);
        // A low end of `a` then all zeros, or a high end of `b` then all nines, joins the middle's free digits.
        const bool lowFull = lowRest == std::string(rest, '0');
        const bool highFull = highRest == std::string(rest, '9');
        const char first = lowFull ? a : static_cast<char>(a + 1);
        const char last = highFull ? b : static_cast<char>(b - 1);
        if (!lowFull) {
            alternatives.push_back(Literal(std::string(1, a)) + " " +
                                   Alternatives(SameLengthAlternatives(lowRest, std::string(rest, '9'))));
        }
        if (first <= last) {
            alternatives.push_back(digitsFrom(first, last) + " " + AnyDigits(rest, rest));
        }
        if (!highFull) {
            alternatives.push_back(Literal(std::string(1, b)) + " " +
                                   Alternatives(SameLengthAlternatives(std::string(rest, '0'), highRest)));
        }
    }
    // The shared prefix is written once, in front of the group.
    if (split == 0) {
        return alternatives;
    }
    return {Literal(low.substr(0, split)) + " " + Alternatives(alternatives)};
}

// The alternatives matching the decimal numbers from lo to hi (1 <= lo <= hi), without leading zeros. Each digit
// count is a band: the shortest and the longest may be partial, every band between them is full, and neighbouring
// full bands share one alternative, a first digit of 1 to 9 and then free digits.
std::vector<std::string> MagnitudeAlternatives(unsigned long long lo, unsigned long long hi)
{
    const std::string low = std::to_string(lo);
    const std::string high = std::to_string(hi);
    if (low.size() == high.size()) {
        return SameLengthAlternatives(low, high);
    }
    std::vector<std::string> alternatives;
    // The shortest band, from lo up to all nines; it is full when lo is a power of ten.
    std::size_t firstFull = low.size(); // The digit count of the first full band.
    if (low != "1" + std::string(low.size() - 1, '0')) {
        alternatives = SameLengthAlternatives(low, std::string(low.size(), '9'));
        firstFull = low.size() + 1;
    }
    // The longest band, from its power of ten up to hi; it is full when hi is all nines.
    const bool highFull = high == std::string(high.size(), '9');
    const std::size_t lastFull = highFull ? high.size() : high.size() - 1;
    if (firstFull <= lastFull) {
        alternatives.push_back(lastFull == 1 ? std::string("[1-9]") : "[1-9] " + AnyDigits(firstFull - 1, lastFull - 1));
    }
    if (!highFull) {
        const std::vector<std::string> top = SameLengthAlternatives("1" + std::string(high.size() - 1, '0'), high);
        alternatives.insert(alternatives.end(), top.begin(), top.end());
    }
    return alternatives;
}

} // namespace
```

and replace

```cpp
std::string Enum(const std::vector<std::string>& values)
{
```

with

```cpp
std::string IntRange(long long min, long long max)
{
    MORPHEUS_REQUIRE(min <= max, "IntRange: min must not exceed max");
    // Magnitudes are unsigned, so the magnitude of the lowest long long (2^63) fits.
    auto magnitude = [](long long value) { return 0ULL - static_cast<unsigned long long>(value); };
    std::vector<std::string> alternatives;
    // The negative numbers: "-" and a magnitude of 1 or more, so "-0" cannot be written.
    if (min < 0) {
        const unsigned long long fewest = max < 0 ? magnitude(max) : 1ULL;
        alternatives.push_back(Literal("-") + " " + Alternatives(MagnitudeAlternatives(fewest, magnitude(min))));
    }
    // Zero is written "0" and nothing else.
    if (min <= 0 && max >= 0) {
        alternatives.push_back(Literal("0"));
    }
    // The positive numbers.
    if (max > 0) {
        const unsigned long long fewest = min > 0 ? static_cast<unsigned long long>(min) : 1ULL;
        const std::vector<std::string> positive = MagnitudeAlternatives(fewest, static_cast<unsigned long long>(max));
        alternatives.insert(alternatives.end(), positive.begin(), positive.end());
    }
    return Alternatives(alternatives);
}

std::string Enum(const std::vector<std::string>& values)
{
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[int-range]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). If a hand-computed text differs, recheck it against the algorithm above before touching the code: the brute force is the arbiter of exactness.

- [ ] **Step 6: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 335`.

- [ ] **Step 7: Commit**

```bash
git add src/grammar/Gbnf.h src/grammar/Gbnf.cpp tests/grammar/GbnfTests.cpp
git commit -m "Add IntRange: exact decimal integer ranges as GBNF" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Morpheus's grammars on the builder

`BuildResolveGrammar`, `BuildLookupGrammar` and `BuildReviewGrammar` are rewritten with `grammar::GrammarBuilder`; the private helpers go. The language does not change (Task 2's characterization tests, the existing accept/reject tests and the replays stay green). Exact-text tests that see a body with alternatives are updated to the builder's parentheses first, and fail against today's code.

**Files:**
- Modify: `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/turn/CMakeLists.txt`
- Test: `tests/turn/ResolveGrammarTests.cpp`

**Interfaces:**
- Consumes: `grammar::Literal`, `JsonString`, `IntRange`, `Enum`, `ListOf`, `Object`, `ObjectField`, `Alternatives`, `GrammarBuilder` (Tasks 3, 4).
- Produces: the same three functions with the same signatures and languages; `constexpr long long turn::MAX_INT_MAGNITUDE = 999999999;`. `morpheus_turn` links `morpheus_grammar`.

- [ ] **Step 1: Update the exact-text tests to the builder's text**

In `tests/turn/ResolveGrammarTests.cpp` make these replacements.

In "The resolve grammar names the world's stats, difficulties and field shapes", replace

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= "\"easy\"" | "\"hard\"")"));
```

with

```cpp
    // Exact text from the grammar builder: an enum's alternatives come in parentheses.
    CHECK_THAT(grammar, ContainsSubstring(R"(difficulty ::= ("\"easy\"" | "\"hard\""))"));
```

and in the same test replace

```cpp
    const std::string items = FindRuleName(grammar, "items", R"("lantern" | item)");
```

with

```cpp
    const std::string items = FindRuleName(grammar, "items", R"(("lantern" | item))"); // The builder's parentheses.
```

Then, in each of the three tests "The resolve grammar names the world's stats, difficulties and field shapes", "The resolve grammar follows the live state and the memory mode" and "The resolve grammar covers list fields and enum sets on existing entities", replace

```cpp
    const std::string disposition = FindRuleName(grammar, "enum", R"("\"wary\"" | "\"friendly\"")");
```

with

```cpp
    const std::string disposition = FindRuleName(grammar, "enum", R"(("\"wary\"" | "\"friendly\""))"); // Builder text.
```

In "The resolve grammar follows the live state and the memory mode", replace

```cpp
    CHECK(CountOccurrences(grammar, R"(::= "\"wary\"")") == 1);
```

with

```cpp
    CHECK(CountOccurrences(grammar, R"(::= ("\"wary\"")") == 1); // The builder's parentheses.
```

In "The resolve grammar bounds every list", replace

```cpp
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= "[]" | "[" check ("," check){0,2} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= "[]" | "[" change ("," change){0,5} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= "[]" | "[" change ("," change){0,3} "]")"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= "[]" | "[" long-text ("," long-text){0,1} "]")"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= "[]" | "[" lookup ("," lookup){0,2} "]")"));
```

with

```cpp
    // Each bound is one less than the maximum: the first element is written out, the repetition adds the rest.
    // Exact text from the grammar builder: a list that may be empty is a group of two alternatives.
    CHECK_THAT(grammar, ContainsSubstring(R"(checks ::= ("[]" | "[" check ("," check){0,2} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(changes ::= ("[]" | "[" change ("," change){0,5} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(branch ::= ("[]" | "[" change ("," change){0,3} "]"))"));
    CHECK_THAT(grammar, ContainsSubstring(R"(notes ::= ("[]" | "[" long-text ("," long-text){0,1} "]"))"));
    CHECK_THAT(turn::BuildLookupGrammar(), ContainsSubstring(R"(lookups ::= ("[]" | "[" lookup ("," lookup){0,2} "]"))"));
```

In "The resolve grammar escapes enum values", replace

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(::= "\"a\\\"b\\\\c\"" | "\"plain\"")"));
```

with

```cpp
    CHECK_THAT(grammar, ContainsSubstring(R"(::= ("\"a\\\"b\\\\c\"" | "\"plain\""))")); // The builder's parentheses.
```

In "Held counter items are matched by name next to the generic item rule", replace

```cpp
    const std::string items =
        FindRuleName(grammar, "items", R"("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item)");
```

with

```cpp
    // Exact text from the grammar builder: the alternatives come in parentheses.
    const std::string items =
        FindRuleName(grammar, "items", R"(("Rusty Knife" | "say \\\"hi\\\"" | "water-flask" | item))");
```

In "The review grammar asks for the verdict, the digest or both", replace

```cpp
    CHECK_THAT(both, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" ",\"digest\":" digest-text "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}")"));
```

with

```cpp
    // Exact text from the grammar builder: two objects as alternatives, each key and value its own fragment.
    CHECK_THAT(both, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" ",\"digest\":" digest-text "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}"))"));
```

and replace

```cpp
    CHECK_THAT(check, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" "}")"));
```

with

```cpp
    CHECK_THAT(check, ContainsSubstring(R"(review ::= ("{\"consistent\":" "true" ",\"problems\":" "[]" "}" | "{\"consistent\":" "false" ",\"problems\":" "[" problem-text ("," problem-text){0,2} "]" "}"))"));
```

- [ ] **Step 2: Run the tests to verify the updated ones fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: failures in "The resolve grammar names the world's stats, difficulties and field shapes", "The resolve grammar follows the live state and the memory mode", "The resolve grammar bounds every list", "The resolve grammar covers list fields and enum sets on existing entities", "The resolve grammar escapes enum values", "Held counter items are matched by name next to the generic item rule" and "The review grammar asks for the verdict, the digest or both" (today's text has no parentheses); the six characterization tests still pass.

- [ ] **Step 3: Link the library into `turn`**

In `src/turn/CMakeLists.txt`, replace

```cmake
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context)
```

with

```cmake
target_link_libraries(morpheus_turn PUBLIC morpheus_core morpheus_store morpheus_inference morpheus_context morpheus_grammar)
```

- [ ] **Step 4: Rewrite the header**

Replace the whole of `src/turn/ResolveGrammar.h` with:

```cpp
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/MemoryMode.h"
#include "core/World.h"

namespace morpheus::turn {

// Size limits the grammars enforce. They bound the resolve output, and so the resolution record, and keep the
// model from rambling inside JSON strings.
constexpr int MAX_CHECKS         = 3;   // Checks per turn.
constexpr int MAX_CHANGES        = 6;   // Unconditional changes.
constexpr int MAX_BRANCH_CHANGES = 4;   // Per check outcome.
constexpr int MAX_NOTES          = 2;   // Assisted mode.
constexpr int MAX_LOOKUPS        = 3;   // Assisted mode.
constexpr int MAX_LIST_ITEMS     = 8;   // List and counter values inside a create.
constexpr int SHORT_TEXT_CHARS   = 120; // intent, stakes, reasons, string values, lookup queries.
constexpr int LONG_TEXT_CHARS    = 400; // Notes.
constexpr int MAX_NEW_ID_CHARS   = 40;
constexpr int MAX_ITEM_CHARS     = 32;  // New counter items; held items are matched by name.
constexpr int REVIEW_MAX_PROBLEMS      = 3;   // Contradictions one review may list.
constexpr int REVIEW_PROBLEM_MAX_CHARS = 200; // Each of them.
constexpr int DIGEST_MAX_CHARS         = 300; // A turn's digest.
constexpr long long MAX_INT_MAGNITUDE  = 999999999; // Integers have at most 9 digits, so they always fit an int.

// The three grammars are built with grammar::GrammarBuilder, so each starts with its root line.

// GBNF for the resolve call (architecture spec, "Turn flow"). The output is one compact JSON object with its keys
// in this order:
//   {"intent": str, "checks": [check], "changes": [change]}, plus "notes": [str] in assisted mode
//   check:  {"stat", "difficulty", "stakes": str, "on_success": [change], "on_failure": [change]}
//   change: {"op", "path", "value" (absent for a counter item remove), "reason": str}
// Only shapes the schema allows can be written: for every field of every entity in `state`, int set/add, enum
// set (its values), string set, list add/remove, counter item add (non-zero) and remove; and create of a
// creatable type with a new id and every field of that type in name order. Integers are exact: from
// -MAX_INT_MAGNITUDE to MAX_INT_MAGNITUDE, without leading zeros or "-0". Counter item names: a new item is
// written as [a-z0-9_]{1,MAX_ITEM_CHARS}; an item the entity holds now is also matched by its own name, verbatim,
// so names such as "Rusty Knife" can be added to and removed. Stats and difficulties are the world's. Applying
// still validates values (ranges, existing ids). `state` must be validated state.
// When no entity has a field and no type is creatable, no change can be written: the grammar's `changes` and
// branch lists can then only be "[]".
std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode);

// GBNF for the assisted-mode lookup call: {"lookups": [{"source": "lore"|"history"|"notes", "query": str}]}, at
// most MAX_LOOKUPS entries. Its first line, the root, is how turn::CallKindOf tells a lookup call.
std::string BuildLookupGrammar();

// GBNF for the review call (architecture spec, "Turn flow"). Its first line is always "root ::= review". The output
// is one compact JSON object:
//   check:  {"consistent":true,"problems":[]} or {"consistent":false,"problems":[1 to REVIEW_MAX_PROBLEMS strings of
//           at most REVIEW_PROBLEM_MAX_CHARS]}
//   digest: "digest": a string of at most DIGEST_MAX_CHARS, after the verdict, or alone: {"digest": str}
// Throws ContractError unless `check` or `digest` is set.
std::string BuildReviewGrammar(bool check, bool digest);

} // namespace morpheus::turn
```

- [ ] **Step 5: Rewrite the builders**

Replace the whole of `src/turn/ResolveGrammar.cpp` with:

```cpp
#include "turn/ResolveGrammar.h"

#include <format>
#include <map>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "grammar/Gbnf.h"

namespace morpheus::turn {

std::string BuildResolveGrammar(const core::World& world, const nlohmann::json& state, core::MemoryMode mode)
{
    MORPHEUS_REQUIRE(state.is_object(), "BuildResolveGrammar: state must be validated state");
    const bool assisted = mode == core::MemoryMode::Assisted;
    grammar::GrammarBuilder builder;

    std::vector<std::string> changes;          // Names of the change-N rules.
    std::map<std::string, std::string> enums;  // "type.field" -> enum-N rule name.
    int itemRuleCount = 0;                     // items-N rules written so far.

    // The value rule for a field; an enum gets its own rule the first time it is used.
    auto valueRule = [&](const std::string& typeName, const std::string& fieldName, const core::FieldDef& def) {
        switch (def.type) {
        case core::FieldType::Int:
            return std::string("int");
        case core::FieldType::String:
            return std::string("short-text");
        case core::FieldType::List:
            return std::string("text-list");
        case core::FieldType::Counter:
            return std::string("counter-value");
        case core::FieldType::Enum: {
            auto it = enums.find(typeName + "." + fieldName);
            if (it == enums.end()) {
                const std::string name = builder.Rule(std::format("enum-{}", enums.size()), grammar::Enum(def.values));
                it = enums.emplace(typeName + "." + fieldName, name).first;
            }
            return it->second;
        }
        }
        throw core::ContractError("BuildResolveGrammar: unhandled FieldType");
    };
    // Each change shape is a rule of its own, change-N, and `change` is any one of them.
    auto addChange = [&](std::string body) {
        changes.push_back(builder.Rule(std::format("change-{}", changes.size()), std::move(body)));
    };

    // Changes to the fields of every entity that exists now. Each body is a sequence of literals around the value
    // rule; the `reason` rule closes the change object.
    for (auto entity = state.begin(); entity != state.end(); ++entity) {
        const std::string typeName = entity.value().at("type").get<std::string>();
        const core::EntityTypeDef* pType = world.schema.FindEntityType(typeName);
        MORPHEUS_REQUIRE(pType != nullptr, "BuildResolveGrammar: state has an unknown entity type");
        for (const auto& [fieldName, def] : pType->fields) {
            const std::string path = entity.key() + "." + fieldName;
            const std::string pathValue = grammar::Literal(",\"path\":\"" + path + "\",\"value\":");
            switch (def.type) {
            case core::FieldType::Int:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"set", "add"}) + " " + pathValue +
                          " int reason");
                break;
            case core::FieldType::Enum:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " " +
                          valueRule(typeName, fieldName, def) + " reason");
                break;
            case core::FieldType::String:
                addChange(grammar::Literal("{\"op\":\"set\"") + " " + pathValue + " short-text reason");
                break;
            case core::FieldType::List:
                addChange(grammar::Literal("{\"op\":") + " " + grammar::Enum({"add", "remove"}) + " " + pathValue +
                          " short-text reason");
                break;
            case core::FieldType::Counter: {
                // The item part of the path: an item held now, written by name (so "Rusty Knife" or "water-flask"
                // can be changed), or the generic rule for a new item. Nothing held means just the generic rule.
                std::string itemRule = "item";
                const nlohmann::json& held = entity.value().at("fields").at(fieldName);
                if (!held.empty()) {
                    std::vector<std::string> names;
                    for (const auto& [itemName, count] : held.items()) {
                        // The name sits inside the JSON path string, so match its JSON-escaped form without the
                        // surrounding quotes, then escape that once more as a GBNF literal.
                        const std::string escaped = nlohmann::json(itemName).dump();
                        names.push_back(grammar::Literal(escaped.substr(1, escaped.size() - 2)));
                    }
                    names.push_back("item");
                    itemRule = builder.Rule(std::format("items-{}", itemRuleCount++), grammar::Alternatives(names));
                }
                // Add a non-zero count to the item, or remove the item (no value).
                addChange(grammar::Alternatives(
                              {grammar::Literal("{\"op\":\"add\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\",\"value\":") + " nonzero-int",
                               grammar::Literal("{\"op\":\"remove\",\"path\":\"" + path + ".") + " " + itemRule + " " +
                                   grammar::Literal("\"")}) +
                          " reason");
                break;
            }
            }
        }
    }

    // New entities of creatable types, every field given, in name order.
    for (const auto& [typeName, type] : world.schema.EntityTypes()) {
        if (!type.creatable) {
            continue;
        }
        std::string fields;
        for (const auto& [fieldName, def] : type.fields) {
            fields += " " + grammar::Literal((fields.empty() ? "\"" : ",\"") + fieldName + "\":") + " " +
                      valueRule(typeName, fieldName, def);
        }
        addChange(grammar::Literal("{\"op\":\"create\",\"path\":") + " new-id " +
                  grammar::Literal(",\"value\":{\"type\":\"" + typeName + "\",\"fields\":{") + fields + " " +
                  grammar::Literal("}}") + " reason");
    }

    std::vector<std::string> difficulties;
    for (const auto& [name, target] : world.difficulties) {
        difficulties.push_back(name);
    }

    // The proposal's structure: checks with their two outcome branches, the unconditional changes and, in assisted
    // mode, notes. With nothing the model could change (no entity fields, no creatable type) the change lists can
    // only be empty and there is no `change` rule to reference.
    builder.Rule("checks", grammar::ListOf("check", 0, MAX_CHECKS));
    builder.Rule("check", grammar::Object({{"stat", "stat"},
                                           {"difficulty", "difficulty"},
                                           {"stakes", "short-text"},
                                           {"on_success", "branch"},
                                           {"on_failure", "branch"}}));
    builder.Rule("branch", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_BRANCH_CHANGES));
    builder.Rule("changes", changes.empty() ? grammar::Literal("[]") : grammar::ListOf("change", 0, MAX_CHANGES));
    if (assisted) {
        builder.Rule("notes", grammar::ListOf("long-text", 0, MAX_NOTES));
        builder.Rule("long-text", grammar::JsonString(0, LONG_TEXT_CHARS));
    }
    builder.Rule("stat", grammar::Enum(world.stats));
    builder.Rule("difficulty", grammar::Enum(difficulties));
    if (!changes.empty()) {
        builder.Rule("change", grammar::Alternatives(changes));
    }

    // The value rules. Integers are exact ranges of at most 9 digits; a counter add is never zero.
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    builder.Rule("int", grammar::IntRange(-MAX_INT_MAGNITUDE, MAX_INT_MAGNITUDE));
    builder.Rule("nonzero-int", grammar::Alternatives({grammar::IntRange(-MAX_INT_MAGNITUDE, -1),
                                                       grammar::IntRange(1, MAX_INT_MAGNITUDE)}));
    builder.Rule("pos-int", grammar::IntRange(1, MAX_INT_MAGNITUDE));
    builder.Rule("item", std::format("[a-z0-9_]{{1,{}}}", MAX_ITEM_CHARS));
    builder.Rule("new-id", grammar::Literal("\"") + std::format(" [a-z0-9_:]{{1,{}}} ", MAX_NEW_ID_CHARS) +
                               grammar::Literal("\""));
    builder.Rule("text-list", grammar::ListOf("short-text", 0, MAX_LIST_ITEMS));
    // A counter value: {} or up to MAX_LIST_ITEMS new items, each with a positive count.
    builder.Rule("counter-value",
                 grammar::Alternatives({grammar::Literal("{}"),
                                        grammar::Literal("{\"") + " item " + grammar::Literal("\":") + " pos-int (" +
                                            grammar::Literal(",\"") + " item " + grammar::Literal("\":") +
                                            std::format(" pos-int){{0,{}}} ", MAX_LIST_ITEMS - 1) + grammar::Literal("}")}));
    // Every change ends with its reason, which closes the change object.
    builder.Rule("reason", grammar::Literal(",\"reason\":") + " short-text " + grammar::Literal("}"));

    std::vector<grammar::ObjectField> root = {{"intent", "short-text"}, {"checks", "checks"}, {"changes", "changes"}};
    if (assisted) {
        root.push_back({"notes", "notes"});
    }
    return builder.Text(grammar::Object(root));
}

std::string BuildLookupGrammar()
{
    grammar::GrammarBuilder builder;
    builder.Rule("lookups", grammar::ListOf("lookup", 0, MAX_LOOKUPS));
    builder.Rule("lookup", grammar::Object({{"source", grammar::Enum({"lore", "history", "notes"})},
                                            {"query", "short-text"}}));
    builder.Rule("short-text", grammar::JsonString(0, SHORT_TEXT_CHARS));
    // The root line, {"lookups": [...]}, is how CallKindOf recognises a lookup call.
    return builder.Text(grammar::Object({{"lookups", "lookups"}}));
}

std::string BuildReviewGrammar(bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "BuildReviewGrammar: needs the check, the digest or both");
    grammar::GrammarBuilder builder;
    std::string review;
    if (check) {
        // Consistent with no problems, or inconsistent with 1 to REVIEW_MAX_PROBLEMS of them; the digest, when it is
        // asked for, follows the verdict.
        std::vector<grammar::ObjectField> consistent = {{"consistent", grammar::Literal("true")},
                                                        {"problems", grammar::Literal("[]")}};
        std::vector<grammar::ObjectField> inconsistent = {
            {"consistent", grammar::Literal("false")},
            {"problems", grammar::ListOf("problem-text", 1, REVIEW_MAX_PROBLEMS)}};
        if (digest) {
            consistent.push_back({"digest", "digest-text"});
            inconsistent.push_back({"digest", "digest-text"});
        }
        review = grammar::Alternatives({grammar::Object(consistent), grammar::Object(inconsistent)});
    } else {
        // The digest alone.
        review = grammar::Object({{"digest", "digest-text"}});
    }
    builder.Rule("review", review);
    if (check) {
        builder.Rule("problem-text", grammar::JsonString(1, REVIEW_PROBLEM_MAX_CHARS));
    }
    if (digest) {
        builder.Rule("digest-text", grammar::JsonString(0, DIGEST_MAX_CHARS));
    }
    // A fixed root line, whatever is asked for, so the call is recognised by it (CallKindOf).
    return builder.Text("review");
}

} // namespace morpheus::turn
```

- [ ] **Step 6: Run the grammar tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar]"`
Expected: builds with no warnings; `All tests passed` (18 test cases: the twelve existing, with their updated text, and the six characterization tests).

- [ ] **Step 7: Run the call-kind, replay, session and review tests**

Run: `./build/tests/bin/Debug/morpheus_tests.exe "[replay],[session],[review],[model_seat]"`
Expected: `All tests passed`. `CallKindOf` still recognises the lookup and review grammars by their first lines (and the model seat labels calls through it), every replay fixture still replays, and the session tests that compare `spec.grammar` with `BuildReviewGrammar(...)` still match.

- [ ] **Step 8: Confirm the old helpers are gone**

Run: `grep -n "Terminals\|TERMINALS\|namespace {" src/turn/ResolveGrammar.cpp`
Expected: no output (no private helpers, no copies).

- [ ] **Step 9: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 335`.

- [ ] **Step 10: Commit**

```bash
git add src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/turn/CMakeLists.txt tests/turn/ResolveGrammarTests.cpp
git commit -m "Build the resolve, lookup and review grammars on the grammar builder" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 6: `FromSchema`

One internal step, `Compile`, reads a schema into a checked tree (`Node`) and throws `DataError` naming the JSON pointer; `FromSchema` writes the tree with the builder. Task 7 adds `Validate` over the same tree.

**Files:**
- Create: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`, `tests/grammar/TestSchemas.h`
- Modify: `src/grammar/CMakeLists.txt`
- Test: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: the builder (Tasks 3, 4); `core::DataError`, `core::ContractError` (`core/Errors.h`); `core::JsonIntInRange` (`core/JsonInt.h`); `test::GbnfMatcher`.
- Produces:
  - `std::string grammar::FromSchema(const nlohmann::json& schema);` (header `grammar/Schema.h`).
  - In `Schema.cpp` (anonymous namespace, used again by Task 7): `struct Node` (kinds `Object`, `Enum`, `Const`, `Integer`, `String`, `Array`, `OneOf`; fields `keys`, `children`, `values`, `minimum`, `maximum`, `minCount`, `maxCount`), `std::string PointerToken(std::string_view key)`, `Node Compile(const nlohmann::json& schema, const std::string& pointer)`.
  - `tests/grammar/TestSchemas.h`: `nlohmann::json test::CatalogSchema()`, `nlohmann::json test::EveryKeywordSchema()`.

- [ ] **Step 1: Write the test schemas**

Create `tests/grammar/TestSchemas.h`:

```cpp
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
```

- [ ] **Step 2: Write the failing tests**

Create `tests/grammar/SchemaTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;
using namespace nlohmann::literals;

namespace {

// The grammar GrammarBuilder writes for the root expression `rootBody`: what FromSchema must give.
std::string GrammarOf(const std::string& rootBody)
{
    return grammar::GrammarBuilder().Text(rootBody);
}

// The message of the DataError FromSchema throws for the schema `text`.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        return error.what();
    }
    FAIL("FromSchema accepted the schema " << text);
    return {};
}

// A oneOf branch with the one key `key`, whose schema is the JSON text `tag`.
std::string Branch(const std::string& key, const std::string& tag)
{
    return R"({"type": "object", "properties": {")" + key + R"(": )" + tag + R"(}, "required": [")" + key + R"("]})";
}

// 48 characters of two bytes each: the longest item the catalog allows.
std::string LongestItem()
{
    std::string item;
    for (int i = 0; i < 48; ++i) {
        item += "\xC3\xA9";
    }
    return item;
}

} // namespace

TEST_CASE("FromSchema writes each keyword with the builder", "[schema]")
{
    CHECK(grammar::FromSchema(R"({"const": "none"})"_json) == GrammarOf(grammar::Literal(R"("none")")));
    CHECK(grammar::FromSchema(R"({"const": -3})"_json) == GrammarOf(grammar::Literal("-3")));
    CHECK(grammar::FromSchema(R"({"enum": ["a", "b"]})"_json) == GrammarOf(grammar::Enum({"a", "b"})));
    CHECK(grammar::FromSchema(R"({"enum": [1, 20]})"_json) ==
          GrammarOf(grammar::Alternatives({grammar::Literal("1"), grammar::Literal("20")})));
    CHECK(grammar::FromSchema(R"({"type": "integer", "minimum": -5, "maximum": 300})"_json) ==
          GrammarOf(grammar::IntRange(-5, 300)));
    CHECK(grammar::FromSchema(R"({"type": "string", "maxLength": 5})"_json) == GrammarOf(grammar::JsonString(0, 5)));
    CHECK(grammar::FromSchema(R"({"type": "string", "minLength": 2, "maxLength": 5})"_json) ==
          GrammarOf(grammar::JsonString(2, 5)));
    CHECK(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "maxItems": 2})"_json) ==
          GrammarOf(grammar::ListOf(grammar::Literal("1"), 0, 2)));
    CHECK(grammar::FromSchema(R"({"type": "array", "items": {"const": 1}, "minItems": 1, "maxItems": 3})"_json) ==
          GrammarOf(grammar::ListOf(grammar::Literal("1"), 1, 3)));
    // Keys follow `required`, not the sorted order nlohmann keeps `properties` in; no keys is the empty object.
    CHECK(grammar::FromSchema(
              R"({"type": "object", "properties": {"b": {"const": 1}, "a": {"const": 2}}, "required": ["b", "a"]})"_json) ==
          GrammarOf(grammar::Object({{"b", grammar::Literal("1")}, {"a", grammar::Literal("2")}})));
    CHECK(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": []})"_json) ==
          GrammarOf(grammar::Literal("{}")));
    CHECK(grammar::FromSchema(nlohmann::json::parse(R"({"oneOf": [)" + Branch("k", R"({"const": "x"})") + ", " +
                                                    Branch("k", R"({"const": "y"})") + "]}")) ==
          GrammarOf(grammar::Alternatives({grammar::Object({{"k", grammar::Literal(R"("x")")}}),
                                           grammar::Object({{"k", grammar::Literal(R"("y")")}})})));
    // One exact text pins the shape: the root line first, and a JSON string brings the shared `char` rule.
    CHECK(grammar::FromSchema(
              R"({"type": "object", "properties": {"name": {"type": "string", "maxLength": 8}}, "required": ["name"]})"_json) ==
          R"gbnf(root ::= "{\"name\":" "\"" char{0,8} "\"" "}"
char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
)gbnf");
}

TEST_CASE("FromSchema ignores the keywords that do not change values", "[schema]")
{
    const nlohmann::json annotated = R"({
        "$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:test", "title": "t",
        "description": "d", "$comment": "c",
        "type": "object", "properties": {"a": {"description": "the a", "const": 1}}, "required": ["a"]
    })"_json;
    CHECK(grammar::FromSchema(annotated) ==
          grammar::FromSchema(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": ["a"]})"_json));
    // additionalProperties false says what the subset always means.
    CHECK(grammar::FromSchema(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": false})"_json) ==
          grammar::FromSchema(R"({"type": "object", "properties": {}, "required": []})"_json));
}

TEST_CASE("The catalog grammar accepts the host's actions and refuses its model's mistakes", "[schema]")
{
    const test::GbnfMatcher matcher(grammar::FromSchema(test::CatalogSchema()));
    const std::string longest = LongestItem();
    const std::vector<std::string> accepted = {
        R"({"action":"none","params":{}})",
        R"({"action":"follow","params":{}})",
        R"({"action":"wait","params":{}})",
        R"({"action":"attack","params":{"target":"0001A6A4"}})",
        R"({"action":"attack","params":{"target":"0001B07C"}})",
        R"({"action":"give_item","params":{"item":"Iron Sword"}})",
        R"({"action":"give_item","params":{"item":""}})",
        R"({"action":"give_item","params":{"item":")" + longest + R"("}})",
        R"({"action":"end_conversation","params":{}})",
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(matcher.Accepts(text));
    }
    const std::vector<std::string> rejected = {
        R"({"action":"dance","params":{}})",                                // an unknown action
        R"({"action":"attack","params":{"target":"0001C3D2"}})",            // an actor who is not present
        R"({"action":"attack","params":{"target":"Saadia"}})",              // a name, not an id
        R"({"action":"attack","params":{}})",                               // a missing argument
        R"({"action":"give_item","params":{"item":")" + longest + R"(a"}})", // 49 characters
        R"({"action":"wait","params":{},"intent":"rest"})",                 // an extra key (the measured Qwen failure)
        R"({"action":"wait","params":{"intent":"rest"}})",                  // an extra argument
        R"({"params":{},"action":"wait"})",                                 // keys out of order
        R"({"action":"wait"})",                                             // no params
        R"({"action": "wait", "params": {}})",                              // whitespace
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }
}

TEST_CASE("The grammar of every keyword accepts a valid value and refuses each broken one", "[schema]")
{
    const test::GbnfMatcher matcher(grammar::FromSchema(test::EveryKeywordSchema()));
    const std::string base =
        R"({"name":"ab","count":0,"big":5,"tags":["red"],"codes":[],"version":2,"pair":[{"x":1,"label":"point"},{"x":2,"label":"point"}],"empty":{},"mode":{"state":"off"}})";
    // `base` with its first `from` replaced by `to`.
    auto with = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    };
    CHECK(matcher.Accepts(base));
    const std::vector<std::string> accepted = {
        with(R"("mode":{"state":"off"})", R"("mode":{"state":"on","level":3})"),
        with(R"("big":5)", R"("big":-9223372036854775808)"),
        with(R"("big":5)", R"("big":9223372036854775807)"),
        with(R"("count":0)", R"("count":-20)"),
        with(R"("count":0)", R"("count":120)"),
        with(R"("codes":[])", R"("codes":[-7,120])"),
        with(R"("tags":["red"])", R"("tags":["red","green","blue"])"),
        with(R"("name":"ab")", R"("name":"\u00e9\"")"),
        with(R"("name":"ab")", "\"name\":\"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\""),
    };
    for (const std::string& text : accepted) {
        INFO(text);
        CHECK(matcher.Accepts(text));
    }
    const std::vector<std::string> rejected = {
        with(R"("count":0)", R"("count":121)"),
        with(R"("count":0)", R"("count":-21)"),
        with(R"("big":5)", R"("big":9223372036854775808)"),
        with(R"("tags":["red"])", R"("tags":[])"),
        with(R"("tags":["red"])", R"("tags":["pink"])"),
        with(R"("codes":[])", R"("codes":[3,3,3])"),
        with(R"("codes":[])", R"("codes":[4])"),
        with(R"("version":2)", R"("version":3)"),
        with(R"(,{"x":2,"label":"point"})", ""),                          // pair has exactly two items
        with(R"({"x":1,"label":"point"})", R"({"label":"point","x":1})"), // keys out of order
        with(R"("empty":{})", R"("empty":{"a":1})"),
        with(R"("mode":{"state":"off"})", R"("mode":{"state":"on"})"),
        with(R"("name":"ab")", R"("name":"a")"),
        with(R"("name":"ab")", R"("name":"abcdefg")"),
        with(R"({"name":"ab","count":0)", R"({"count":0,"name":"ab")"),
    };
    for (const std::string& text : rejected) {
        INFO(text);
        CHECK_FALSE(matcher.Accepts(text));
    }
}

TEST_CASE("FromSchema refuses what is not a schema of the subset, naming the pointer", "[schema]")
{
    CHECK(SchemaError("[]") == "schema at the root: a schema must be a JSON object");
    CHECK(SchemaError("{}") == "schema at the root: needs 'type', 'enum', 'const' or 'oneOf'");
    CHECK(SchemaError(R"({"type": "string", "maxLength": 3, "enum": ["a"]})") ==
          "schema at the root: 'type' and 'enum' cannot be combined");
    CHECK(SchemaError(R"({"type": "string", "maxLength": 3, "pattern": "a+"})") ==
          "schema at /pattern: the keyword 'pattern' is not supported");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"$ref": "#/$defs/a"}}, "required": ["a"]})") ==
          "schema at /properties/a/$ref: the keyword '$ref' is not supported");
    CHECK(SchemaError(R"({"const": "a", "description": "fine", "default": "a"})") ==
          "schema at /default: the keyword 'default' is not supported");
    CHECK(SchemaError(R"({"type": "boolean"})") == "schema at /type: the type 'boolean' is not supported");
    CHECK(SchemaError(R"({"type": "number"})") == "schema at /type: the type 'number' is not supported");
    CHECK(SchemaError(R"({"type": ["string", "null"], "maxLength": 3})") == "schema at /type: 'type' must be a string");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 3, "maxLength": 2})") ==
          "schema at /maxLength: 'maxLength' does not apply to an integer");
}

TEST_CASE("FromSchema refuses objects outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "object", "required": []})") ==
          "schema at the root: an object needs 'properties', an object");
    CHECK(SchemaError(R"({"type": "object", "properties": {}})") ==
          "schema at the root: an object needs 'required', an array");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": []})") ==
          "schema at /properties/a: 'a' must be in 'required': optional keys are not supported");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": ["a"]})") ==
          "schema at /required/0: 'a' is required but not in 'properties'");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": ["a", "a"]})") ==
          "schema at /required/1: 'a' is listed twice");
    CHECK(SchemaError(R"({"type": "object", "properties": {"a": {"const": 1}}, "required": [1]})") ==
          "schema at /required/0: 'required' entries must be strings");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": [], "additionalProperties": true})") ==
          "schema at /additionalProperties: 'additionalProperties' may only be false: extra keys are never allowed");
    CHECK(SchemaError(R"({"type": "object", "properties": {}, "required": [], "minItems": 1})") ==
          "schema at /minItems: 'minItems' does not apply to an object");
    // A key with "/" and "~" is escaped in the pointer as RFC 6901 says.
    CHECK(SchemaError(R"({"type": "object", "properties": {"a/b~c": {"type": "string"}}, "required": ["a/b~c"]})") ==
          "schema at /properties/a~1b~0c: a string needs 'maxLength'");
}

TEST_CASE("FromSchema refuses enums outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"enum": []})") == "schema at /enum: 'enum' must be a non-empty array");
    CHECK(SchemaError(R"({"enum": "a"})") == "schema at /enum: 'enum' must be a non-empty array");
    CHECK(SchemaError(R"({"enum": ["a", 1]})") == "schema at /enum/1: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": [1, 2.5]})") == "schema at /enum/1: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": [true]})") == "schema at /enum/0: 'enum' values must be all strings or all integers");
    CHECK(SchemaError(R"({"enum": ["a", "b", "a"]})") == R"(schema at /enum/2: "a" is listed twice)");
    CHECK(SchemaError(R"({"enum": ["a"], "maxLength": 3})") == "schema at /maxLength: 'maxLength' does not apply to an enum");
}

TEST_CASE("FromSchema refuses consts outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"const": 1.5})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": null})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": {"a": 1}})") == "schema at /const: 'const' must be a string or an integer");
    CHECK(SchemaError(R"({"const": "a", "maxLength": 3})") == "schema at /maxLength: 'maxLength' does not apply to a const");
}

TEST_CASE("FromSchema refuses integers outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0})") ==
          "schema at the root: an integer needs 'minimum' and 'maximum'");
    CHECK(SchemaError(R"({"type": "integer", "maximum": 0})") ==
          "schema at the root: an integer needs 'minimum' and 'maximum'");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 5, "maximum": 3})") ==
          "schema at the root: 'minimum' 5 is above 'maximum' 3");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0.5, "maximum": 3})") ==
          "schema at /minimum: 'minimum' must be an integer from -9223372036854775808 to 9223372036854775807");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 18446744073709551615})") ==
          "schema at /maximum: 'maximum' must be an integer from -9223372036854775808 to 9223372036854775807");
    CHECK(SchemaError(R"({"type": "integer", "minimum": 0, "maximum": 3, "exclusiveMaximum": 4})") ==
          "schema at /exclusiveMaximum: the keyword 'exclusiveMaximum' is not supported");
}

TEST_CASE("FromSchema refuses strings outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "string"})") == "schema at the root: a string needs 'maxLength'");
    CHECK(SchemaError(R"({"type": "string", "minLength": 4, "maxLength": 3})") ==
          "schema at the root: 'minLength' 4 is above 'maxLength' 3");
    CHECK(SchemaError(R"({"type": "string", "maxLength": -1})") ==
          "schema at /maxLength: 'maxLength' must be an integer from 0 to 2147483647");
    CHECK(SchemaError(R"({"type": "string", "maxLength": "3"})") ==
          "schema at /maxLength: 'maxLength' must be an integer from 0 to 2147483647");
}

TEST_CASE("FromSchema refuses arrays outside the subset", "[schema]")
{
    CHECK(SchemaError(R"({"type": "array", "maxItems": 3})") == "schema at the root: an array needs 'items'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}})") == "schema at the root: an array needs 'maxItems'");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "maxItems": 0})") ==
          "schema at /maxItems: 'maxItems' must be at least 1");
    CHECK(SchemaError(R"({"type": "array", "items": {"const": 1}, "minItems": 4, "maxItems": 3})") ==
          "schema at the root: 'minItems' 4 is above 'maxItems' 3");
    CHECK(SchemaError(R"({"type": "array", "items": {"type": "string"}, "maxItems": 3})") ==
          "schema at /items: a string needs 'maxLength'");
}

TEST_CASE("FromSchema refuses oneOfs outside the subset", "[schema]")
{
    const std::string x = Branch("action", R"({"const": "x"})");
    const std::string y = Branch("action", R"({"const": "y"})");
    CHECK(SchemaError(R"({"oneOf": []})") == "schema at /oneOf: 'oneOf' must be a non-empty array");
    CHECK(SchemaError(R"({"oneOf": [)" + x + R"(, {"const": "x"}]})") ==
          "schema at /oneOf/1: a 'oneOf' branch must be an object schema");
    CHECK(SchemaError(R"({"oneOf": [{"type": "object", "properties": {}, "required": []}]})") ==
          "schema at /oneOf/0/required: a 'oneOf' branch needs a discriminator, the first key of 'required'");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("kind", R"({"const": "y"})") + "]}") ==
          "schema at /oneOf/1/required/0: the discriminator is 'kind' here but 'action' in /oneOf/0");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("action", R"({"enum": ["y"]})") + "]}") ==
          "schema at /oneOf/1/properties/action: the discriminator 'action' must be a 'const' string");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + Branch("action", R"({"const": 1})") + "]}") ==
          "schema at /oneOf/1/properties/action: the discriminator 'action' must be a 'const' string");
    CHECK(SchemaError(R"({"oneOf": [)" + x + ", " + y + ", " + x + "]}") ==
          "schema at /oneOf/2/properties/action/const: the discriminator value 'x' is repeated: /oneOf/0 has it too");
    CHECK(SchemaError(R"({"type": "object", "oneOf": [)" + x + "]}") ==
          "schema at the root: 'type' and 'oneOf' cannot be combined");
}

TEST_CASE("FromSchema names the pointer of a mistake deep inside the schema", "[schema]")
{
    // The catalog's attack target with no maxLength: the spec's example pointer.
    nlohmann::json schema = test::CatalogSchema();
    schema["oneOf"][3]["properties"]["params"]["properties"]["target"] = nlohmann::json::parse(R"({"type": "string"})");
    CHECK(SchemaError(schema.dump()) ==
          "schema at /oneOf/3/properties/params/properties/target: a string needs 'maxLength'");
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/Schema.h': No such file or directory` in `tests/grammar/SchemaTests.cpp`.

- [ ] **Step 4: Add the source to the library**

In `src/grammar/CMakeLists.txt`, replace

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
)
```

with

```cmake
add_library(morpheus_grammar STATIC
    Gbnf.cpp
    Schema.cpp
)
```

- [ ] **Step 5: Write the header**

Create `src/grammar/Schema.h`:

```cpp
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
// same rules at any depth; "$ref" is not supported. Anything else is a core::DataError (schemas are host data) whose
// message names the JSON pointer inside the schema and the problem: "schema at <pointer>: <problem>".
std::string FromSchema(const nlohmann::json& schema);

} // namespace morpheus::grammar
```

- [ ] **Step 6: Write the implementation**

Create `src/grammar/Schema.cpp`:

```cpp
#include "grammar/Schema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"

namespace morpheus::grammar {

namespace {

// Keywords that do not change the values a schema allows: accepted anywhere and ignored.
constexpr std::array<std::string_view, 5> IGNORED_KEYWORDS = {"title", "description", "$comment", "$schema", "$id"};
// Every keyword of the subset; any other is refused by name.
constexpr std::array<std::string_view, 14> SUBSET_KEYWORDS = {
    "type",    "enum",    "const",     "oneOf",     "properties", "required", "additionalProperties",
    "minimum", "maximum", "minLength", "maxLength", "items",      "minItems", "maxItems"};
// The keywords that say what a value is; a schema has exactly one of them.
constexpr std::array<std::string_view, 4> KIND_KEYWORDS = {"type", "enum", "const", "oneOf"};

// A schema checked against the subset, as FromSchema and Validate read it.
struct Node {
    enum class Kind { Object, Enum, Const, Integer, String, Array, OneOf };

    Kind kind = Kind::Object;
    std::vector<std::string> keys = {};      // Object: its keys, in `required` order.
    std::vector<Node> children = {};         // Object: one value schema per key; Array: the item schema;
                                             // OneOf: the branches, each an Object whose first key is the discriminator.
    std::vector<nlohmann::json> values = {}; // Enum: the allowed values (all strings or all integers); Const: the one.
    long long minimum = 0;                   // Integer: the bounds, both included.
    long long maximum = 0;
    int minCount = 0;                        // String: characters; Array: items.
    int maxCount = 0;
};

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

// Reads `schema`, found at `pointer` inside the whole schema, into a Node, checking it against the subset. Throws
// DataError naming the pointer of the mistake. Recursive: the schemas of an object's properties, an array's items and
// a oneOf's branches follow the same rules.
Node Compile(const nlohmann::json& schema, const std::string& pointer)
{
    auto error = [](const std::string& at, const std::string& problem) {
        return core::DataError(std::format("schema at {}: {}", at.empty() ? "the root" : at, problem));
    };
    auto contains = [](const auto& keywords, const std::string& key) {
        return std::find(keywords.begin(), keywords.end(), key) != keywords.end();
    };
    if (!schema.is_object()) {
        throw error(pointer, "a schema must be a JSON object");
    }

    // Keywords outside the subset are named first, so that "$ref" or "pattern" is reported as such.
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        if (!contains(SUBSET_KEYWORDS, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
            throw error(pointer + "/" + PointerToken(it.key()), std::format("the keyword '{}' is not supported", it.key()));
        }
    }
    // Exactly one keyword says what the value is.
    std::vector<std::string> kinds;
    for (const std::string_view keyword : KIND_KEYWORDS) {
        if (schema.contains(std::string(keyword))) {
            kinds.emplace_back(keyword);
        }
    }
    if (kinds.empty()) {
        throw error(pointer, "needs 'type', 'enum', 'const' or 'oneOf'");
    }
    if (kinds.size() > 1) {
        throw error(pointer, std::format("'{}' and '{}' cannot be combined", kinds[0], kinds[1]));
    }
    const std::string kind = kinds[0];

    // Only this kind's keywords, and the ignored ones, may appear.
    auto allowOnly = [&](std::string_view what, std::initializer_list<std::string_view> allowed) {
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            if (!contains(allowed, it.key()) && !contains(IGNORED_KEYWORDS, it.key())) {
                throw error(pointer + "/" + PointerToken(it.key()), std::format("'{}' does not apply to {}", it.key(), what));
            }
        }
    };
    // A bound keyword: absent (nullopt), or a JSON integer from low to high.
    auto integer = [&](const char* keyword, long long low, long long high) -> std::optional<long long> {
        if (!schema.contains(keyword)) {
            return std::nullopt;
        }
        const std::optional<long long> value = core::JsonIntInRange(schema.at(keyword), low, high);
        if (!value) {
            throw error(pointer + "/" + keyword, std::format("'{}' must be an integer from {} to {}", keyword, low, high));
        }
        return value;
    };
    constexpr long long LOWEST = std::numeric_limits<long long>::min();
    constexpr long long HIGHEST = std::numeric_limits<long long>::max();
    constexpr long long MOST_COUNT = std::numeric_limits<int>::max();

    Node node;
    if (kind == "oneOf") {
        allowOnly("a oneOf", {"oneOf"});
        const nlohmann::json& branches = schema.at("oneOf");
        const std::string at = pointer + "/oneOf";
        if (!branches.is_array() || branches.empty()) {
            throw error(at, "'oneOf' must be a non-empty array");
        }
        node.kind = Node::Kind::OneOf;
        std::map<std::string, std::string> taken; // Discriminator value -> the branch that has it.
        for (std::size_t i = 0; i < branches.size(); ++i) {
            const nlohmann::json& branch = branches[i];
            const std::string branchAt = std::format("{}/{}", at, i);
            if (!branch.is_object() || !branch.contains("type") || branch.at("type") != "object") {
                throw error(branchAt, "a 'oneOf' branch must be an object schema");
            }
            Node compiled = Compile(branch, branchAt);
            // The discriminator: the first key of the branch's `required`, the same key in every branch, and a const
            // string in each that no other branch has.
            if (compiled.keys.empty()) {
                throw error(branchAt + "/required", "a 'oneOf' branch needs a discriminator, the first key of 'required'");
            }
            const std::string& key = compiled.keys[0];
            if (i > 0 && key != node.children[0].keys[0]) {
                throw error(branchAt + "/required/0", std::format("the discriminator is '{}' here but '{}' in {}/0", key,
                                                                  node.children[0].keys[0], at));
            }
            const Node& tag = compiled.children[0];
            const std::string tagAt = branchAt + "/properties/" + PointerToken(key);
            if (tag.kind != Node::Kind::Const || !tag.values[0].is_string()) {
                throw error(tagAt, std::format("the discriminator '{}' must be a 'const' string", key));
            }
            const std::string value = tag.values[0].get<std::string>();
            if (const auto found = taken.find(value); found != taken.end()) {
                throw error(tagAt + "/const",
                            std::format("the discriminator value '{}' is repeated: {} has it too", value, found->second));
            }
            taken.emplace(value, branchAt);
            node.children.push_back(std::move(compiled));
        }
        return node;
    }

    if (kind == "enum") {
        allowOnly("an enum", {"enum"});
        const nlohmann::json& values = schema.at("enum");
        const std::string at = pointer + "/enum";
        if (!values.is_array() || values.empty()) {
            throw error(at, "'enum' must be a non-empty array");
        }
        // All strings or all integers (a bool is neither), each listed once.
        const bool strings = values[0].is_string();
        for (std::size_t i = 0; i < values.size(); ++i) {
            const nlohmann::json& value = values[i];
            const std::string valueAt = std::format("{}/{}", at, i);
            if (strings ? !value.is_string() : !value.is_number_integer()) {
                throw error(valueAt, "'enum' values must be all strings or all integers");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (values[j] == value) {
                    throw error(valueAt, std::format("{} is listed twice", value.dump()));
                }
            }
        }
        node.kind = Node::Kind::Enum;
        node.values.assign(values.begin(), values.end());
        return node;
    }

    if (kind == "const") {
        allowOnly("a const", {"const"});
        const nlohmann::json& value = schema.at("const");
        if (!value.is_string() && !value.is_number_integer()) {
            throw error(pointer + "/const", "'const' must be a string or an integer");
        }
        node.kind = Node::Kind::Const;
        node.values.push_back(value);
        return node;
    }

    // The rest is told by "type".
    const nlohmann::json& type = schema.at("type");
    if (!type.is_string()) {
        throw error(pointer + "/type", "'type' must be a string");
    }
    const std::string typeName = type.get<std::string>();

    if (typeName == "integer") {
        allowOnly("an integer", {"type", "minimum", "maximum"});
        const std::optional<long long> minimum = integer("minimum", LOWEST, HIGHEST);
        const std::optional<long long> maximum = integer("maximum", LOWEST, HIGHEST);
        if (!minimum || !maximum) {
            throw error(pointer, "an integer needs 'minimum' and 'maximum'");
        }
        if (*minimum > *maximum) {
            throw error(pointer, std::format("'minimum' {} is above 'maximum' {}", *minimum, *maximum));
        }
        node.kind = Node::Kind::Integer;
        node.minimum = *minimum;
        node.maximum = *maximum;
        return node;
    }

    if (typeName == "string") {
        allowOnly("a string", {"type", "minLength", "maxLength"});
        const std::optional<long long> minLength = integer("minLength", 0, MOST_COUNT);
        const std::optional<long long> maxLength = integer("maxLength", 0, MOST_COUNT);
        if (!maxLength) {
            throw error(pointer, "a string needs 'maxLength'");
        }
        if (minLength.value_or(0) > *maxLength) {
            throw error(pointer, std::format("'minLength' {} is above 'maxLength' {}", *minLength, *maxLength));
        }
        node.kind = Node::Kind::String;
        node.minCount = static_cast<int>(minLength.value_or(0));
        node.maxCount = static_cast<int>(*maxLength);
        return node;
    }

    if (typeName == "array") {
        allowOnly("an array", {"type", "items", "minItems", "maxItems"});
        if (!schema.contains("items")) {
            throw error(pointer, "an array needs 'items'");
        }
        const std::optional<long long> minItems = integer("minItems", 0, MOST_COUNT);
        const std::optional<long long> maxItems = integer("maxItems", 0, MOST_COUNT);
        if (!maxItems) {
            throw error(pointer, "an array needs 'maxItems'");
        }
        if (*maxItems < 1) {
            throw error(pointer + "/maxItems", "'maxItems' must be at least 1");
        }
        if (minItems.value_or(0) > *maxItems) {
            throw error(pointer, std::format("'minItems' {} is above 'maxItems' {}", *minItems, *maxItems));
        }
        node.kind = Node::Kind::Array;
        node.minCount = static_cast<int>(minItems.value_or(0));
        node.maxCount = static_cast<int>(*maxItems);
        node.children.push_back(Compile(schema.at("items"), pointer + "/items"));
        return node;
    }

    if (typeName == "object") {
        allowOnly("an object", {"type", "properties", "required", "additionalProperties"});
        if (schema.contains("additionalProperties") && schema.at("additionalProperties") != false) {
            throw error(pointer + "/additionalProperties",
                        "'additionalProperties' may only be false: extra keys are never allowed");
        }
        if (!schema.contains("properties") || !schema.at("properties").is_object()) {
            throw error(pointer, "an object needs 'properties', an object");
        }
        if (!schema.contains("required") || !schema.at("required").is_array()) {
            throw error(pointer, "an object needs 'required', an array");
        }
        const nlohmann::json& properties = schema.at("properties");
        const nlohmann::json& required = schema.at("required");
        node.kind = Node::Kind::Object;
        // The keys in `required` order, which the grammar writes them in (JSON objects carry no order).
        for (std::size_t i = 0; i < required.size(); ++i) {
            const std::string at = std::format("{}/required/{}", pointer, i);
            if (!required[i].is_string()) {
                throw error(at, "'required' entries must be strings");
            }
            const std::string key = required[i].get<std::string>();
            if (!properties.contains(key)) {
                throw error(at, std::format("'{}' is required but not in 'properties'", key));
            }
            if (std::find(node.keys.begin(), node.keys.end(), key) != node.keys.end()) {
                throw error(at, std::format("'{}' is listed twice", key));
            }
            node.keys.push_back(key);
            node.children.push_back(Compile(properties.at(key), pointer + "/properties/" + PointerToken(key)));
        }
        // Every property is required: optional keys are outside the subset.
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                throw error(pointer + "/properties/" + PointerToken(it.key()),
                            std::format("'{}' must be in 'required': optional keys are not supported", it.key()));
            }
        }
        return node;
    }

    throw error(pointer + "/type", std::format("the type '{}' is not supported", typeName));
}

// The grammar expression for the values `node` allows. Recursive, like Compile.
std::string Expression(const Node& node)
{
    switch (node.kind) {
    case Node::Kind::Object: {
        // An object without keys is the empty object (Object needs at least one field).
        if (node.keys.empty()) {
            return Literal("{}");
        }
        std::vector<ObjectField> fields;
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            fields.push_back({node.keys[i], Expression(node.children[i])});
        }
        return Object(fields);
    }
    case Node::Kind::Enum: {
        // Strings as JSON strings (Enum); integers as their decimal text.
        if (node.values[0].is_string()) {
            std::vector<std::string> strings;
            for (const nlohmann::json& value : node.values) {
                strings.push_back(value.get<std::string>());
            }
            return Enum(strings);
        }
        std::vector<std::string> numbers;
        for (const nlohmann::json& value : node.values) {
            numbers.push_back(Literal(value.dump()));
        }
        return Alternatives(numbers);
    }
    case Node::Kind::Const:
        return Literal(node.values[0].dump());
    case Node::Kind::Integer:
        return IntRange(node.minimum, node.maximum);
    case Node::Kind::String:
        return JsonString(node.minCount, node.maxCount);
    case Node::Kind::Array:
        return ListOf(Expression(node.children[0]), node.minCount, node.maxCount);
    case Node::Kind::OneOf: {
        std::vector<std::string> branches;
        for (const Node& branch : node.children) {
            branches.push_back(Expression(branch));
        }
        return Alternatives(branches);
    }
    }
    throw core::ContractError("FromSchema: unhandled schema kind");
}

} // namespace

std::string FromSchema(const nlohmann::json& schema)
{
    // Check the whole schema first, then write it: a bad schema never yields part of a grammar.
    const Node root = Compile(schema, "");
    return GrammarBuilder().Text(Expression(root));
}

} // namespace morpheus::grammar
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed` (13 test cases).

- [ ] **Step 8: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 348`.

- [ ] **Step 9: Commit**

```bash
git add src/grammar/CMakeLists.txt src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/TestSchemas.h tests/grammar/SchemaTests.cpp
git commit -m "Add FromSchema: GBNF for a subset of JSON Schema" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 7: `Validate`

`Validate` compiles the schema exactly as `FromSchema` does (so it throws the same `DataError`) and walks the tree with the value, adding every mistake as a `ValidationError`.

The message formats (the spec asks that they name the expected shape):
- object: `expected an object with the keys 'a', 'b'` / `expected an empty object`; `missing key 'target'` (at the key's path); `unexpected key 'intent'; the keys are 'action', 'params'` / `unexpected key 'intent'; the object must be empty` (at the extra key's path).
- enum: `'Saadia' is not one of: "0001A6A4", "0001B07C"`; const: `expected 2, got 3`.
- integer: `expected an integer from 1 to 5, got 2.5`.
- string: `expected a string of at most 48 characters, got 5` / `of 2 to 6 characters`; `longer than 48 characters`; `shorter than 2 characters`.
- array: `expected an array of 0 to 2 items, got '3'`; `has 4 items; expected 1 to 3`.
- oneOf: `expected an object whose 'action' is one of: "none", ...`; `missing key 'action'`; `unknown action 'dance'; expected one of: "none", ...` (both at the discriminator's path).
- A value is shown as a string in single quotes, anything else as compact JSON; both are cut at 80 bytes (`core::SHORT_JSON_CHARS`) with `...`.

**Files:**
- Modify: `src/grammar/Schema.h`, `src/grammar/Schema.cpp`
- Test: `tests/grammar/SchemaTests.cpp`

**Interfaces:**
- Consumes: Task 6's `Node`, `Compile`, `PointerToken`; `core::Utf8Length`, `core::CutUtf8`, `core::SHORT_JSON_CHARS` (`core/Text.h`); `core::JsonIntInRange`.
- Produces: `struct grammar::ValidationError { std::string path = {}; std::string message = {}; };` and `std::vector<grammar::ValidationError> grammar::Validate(const nlohmann::json& value, const nlohmann::json& schema);`.

- [ ] **Step 1: Make every schema-error test check `Validate` too**

In `tests/grammar/SchemaTests.cpp`, replace

```cpp
// The message of the DataError FromSchema throws for the schema `text`.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        return error.what();
    }
    FAIL("FromSchema accepted the schema " << text);
    return {};
}
```

with

```cpp
// The message of the DataError FromSchema throws for the schema `text`, after checking that Validate throws the same.
std::string SchemaError(const std::string& text)
{
    const nlohmann::json schema = nlohmann::json::parse(text);
    std::string fromSchema;
    try {
        grammar::FromSchema(schema);
    } catch (const core::DataError& error) {
        fromSchema = error.what();
    }
    std::string validate;
    try {
        grammar::Validate(nlohmann::json::object(), schema);
    } catch (const core::DataError& error) {
        validate = error.what();
    }
    INFO("schema: " << text);
    REQUIRE_FALSE(fromSchema.empty());
    CHECK(validate == fromSchema);
    return fromSchema;
}

// The one error Validate finds in `value` against `schema`.
grammar::ValidationError OnlyError(const nlohmann::json& value, const nlohmann::json& schema)
{
    const std::vector<grammar::ValidationError> errors = grammar::Validate(value, schema);
    INFO("value: " << value.dump());
    for (const grammar::ValidationError& error : errors) {
        UNSCOPED_INFO(error.path << ": " << error.message);
    }
    REQUIRE(errors.size() == 1);
    return errors[0];
}
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/grammar/SchemaTests.cpp`:

```cpp
TEST_CASE("Validate accepts every action the catalog allows", "[schema]")
{
    const nlohmann::json schema = test::CatalogSchema();
    const std::vector<std::string> valid = {
        R"({"action":"none","params":{}})",
        R"({"action":"follow","params":{}})",
        R"({"action":"wait","params":{}})",
        R"({"action":"attack","params":{"target":"0001A6A4"}})",
        R"({"action":"give_item","params":{"item":"Iron Sword"}})",
        R"({"action":"give_item","params":{"item":")" + LongestItem() + R"("}})",
        R"({"action":"end_conversation","params":{}})",
    };
    for (const std::string& text : valid) {
        INFO(text);
        CHECK(grammar::Validate(nlohmann::json::parse(text), schema).empty());
    }
}

TEST_CASE("Validate reports each mistake in a catalog action at its path", "[schema]")
{
    const nlohmann::json schema = test::CatalogSchema();
    const std::string actions = R"("none", "follow", "wait", "attack", "give_item", "end_conversation")";

    // An unknown action: one error, at the discriminator.
    const grammar::ValidationError dance = OnlyError(R"({"action":"dance","params":{}})"_json, schema);
    CHECK(dance.path == "/action");
    CHECK(dance.message == "unknown action 'dance'; expected one of: " + actions);

    // An actor who is not present, and a name where an id belongs.
    const grammar::ValidationError absent = OnlyError(R"({"action":"attack","params":{"target":"0001C3D2"}})"_json, schema);
    CHECK(absent.path == "/params/target");
    CHECK(absent.message == R"('0001C3D2' is not one of: "0001A6A4", "0001B07C")");
    CHECK(OnlyError(R"({"action":"attack","params":{"target":"Saadia"}})"_json, schema).message ==
          R"('Saadia' is not one of: "0001A6A4", "0001B07C")");

    // A missing argument.
    const grammar::ValidationError missing = OnlyError(R"({"action":"attack","params":{}})"_json, schema);
    CHECK(missing.path == "/params/target");
    CHECK(missing.message == "missing key 'target'");

    // An item over 48 characters.
    const grammar::ValidationError item = OnlyError(
        nlohmann::json::parse(R"({"action":"give_item","params":{"item":")" + LongestItem() + R"(a"}})"), schema);
    CHECK(item.path == "/params/item");
    CHECK(item.message == "longer than 48 characters");

    // An extra key, the host's measured Qwen failure: "intent" added to wait, at the top or among the arguments.
    const grammar::ValidationError intent = OnlyError(R"({"action":"wait","intent":"rest","params":{}})"_json, schema);
    CHECK(intent.path == "/intent");
    CHECK(intent.message == "unexpected key 'intent'; the keys are 'action', 'params'");
    const grammar::ValidationError argument = OnlyError(R"({"action":"wait","params":{"intent":"rest"}})"_json, schema);
    CHECK(argument.path == "/params/intent");
    CHECK(argument.message == "unexpected key 'intent'; the object must be empty");

    // No discriminator, or no object at all.
    const grammar::ValidationError noAction = OnlyError(R"({"params":{}})"_json, schema);
    CHECK(noAction.path == "/action");
    CHECK(noAction.message == "missing key 'action'");
    const grammar::ValidationError notObject = OnlyError(R"("wait")"_json, schema);
    CHECK(notObject.path == "");
    CHECK(notObject.message == "expected an object whose 'action' is one of: " + actions);
}

TEST_CASE("Validate wants a JSON integer where the schema says integer", "[schema]")
{
    const nlohmann::json schema =
        R"({"type": "object", "properties": {"count": {"type": "integer", "minimum": 1, "maximum": 5}}, "required": ["count"]})"_json;
    CHECK(grammar::Validate(R"({"count": 5})"_json, schema).empty());
    const grammar::ValidationError fraction = OnlyError(R"({"count": 2.5})"_json, schema);
    CHECK(fraction.path == "/count");
    CHECK(fraction.message == "expected an integer from 1 to 5, got 2.5");
    // A float with an integral value is still a float.
    CHECK(OnlyError(R"({"count": 2.0})"_json, schema).message == "expected an integer from 1 to 5, got 2.0");
    CHECK(OnlyError(R"({"count": "2"})"_json, schema).message == "expected an integer from 1 to 5, got '2'");
    CHECK(OnlyError(R"({"count": true})"_json, schema).message == "expected an integer from 1 to 5, got true");
    CHECK(OnlyError(R"({"count": 6})"_json, schema).message == "expected an integer from 1 to 5, got 6");
}

TEST_CASE("Validate reports every error, in document order", "[schema]")
{
    const nlohmann::json schema = R"({
        "type": "object",
        "properties": {
            "name": {"type": "string", "maxLength": 3},
            "tags": {"type": "array", "items": {"enum": ["a", "b"]}, "maxItems": 2},
            "level": {"type": "integer", "minimum": 0, "maximum": 3}
        },
        "required": ["name", "tags", "level"]
    })"_json;
    const nlohmann::json value = R"({"level": 7, "zz": true, "tags": ["a", "c", "d"], "extra": 1, "name": "long name"})"_json;
    const std::vector<grammar::ValidationError> errors = grammar::Validate(value, schema);
    // The schema's keys in its order (the order the grammar writes them), then the keys it does not allow.
    REQUIRE(errors.size() == 7);
    CHECK(errors[0].path == "/name");
    CHECK(errors[0].message == "longer than 3 characters");
    CHECK(errors[1].path == "/tags");
    CHECK(errors[1].message == "has 3 items; expected 0 to 2");
    CHECK(errors[2].path == "/tags/1");
    CHECK(errors[2].message == R"('c' is not one of: "a", "b")");
    CHECK(errors[3].path == "/tags/2");
    CHECK(errors[3].message == R"('d' is not one of: "a", "b")");
    CHECK(errors[4].path == "/level");
    CHECK(errors[4].message == "expected an integer from 0 to 3, got 7");
    CHECK(errors[5].path == "/extra");
    CHECK(errors[5].message == "unexpected key 'extra'; the keys are 'name', 'tags', 'level'");
    CHECK(errors[6].path == "/zz");
    CHECK(errors[6].message == "unexpected key 'zz'; the keys are 'name', 'tags', 'level'");
}

TEST_CASE("Validate checks every keyword of the subset", "[schema]")
{
    const nlohmann::json schema = test::EveryKeywordSchema();
    const nlohmann::json base =
        R"({"name":"ab","count":0,"big":5,"tags":["red"],"codes":[],"version":2,"pair":[{"x":1,"label":"point"},{"x":2,"label":"point"}],"empty":{},"mode":{"state":"off"}})"_json;
    CHECK(grammar::Validate(base, schema).empty());
    // `base` with `key` set to `replacement` gives exactly one error, at `path`, saying `message`.
    auto expectOne = [&](const char* key, const nlohmann::json& replacement, const std::string& path,
                         const std::string& message) {
        nlohmann::json value = base;
        value[key] = replacement;
        INFO(key << " = " << replacement.dump());
        const grammar::ValidationError found = OnlyError(value, schema);
        CHECK(found.path == path);
        CHECK(found.message == message);
    };
    expectOne("version", 3, "/version", "expected 2, got 3");
    expectOne("count", 121, "/count", "expected an integer from -20 to 120, got 121");
    expectOne("big", 1.5, "/big", "expected an integer from -9223372036854775808 to 9223372036854775807, got 1.5");
    expectOne("name", "a", "/name", "shorter than 2 characters");
    expectOne("name", "abcdefg", "/name", "longer than 6 characters");
    expectOne("name", 5, "/name", "expected a string of 2 to 6 characters, got 5");
    expectOne("tags", nlohmann::json::array(), "/tags", "has 0 items; expected 1 to 3");
    expectOne("tags", R"(["pink"])"_json, "/tags/0", R"('pink' is not one of: "red", "green", "blue")");
    expectOne("codes", R"([3.0])"_json, "/codes/0", "3.0 is not one of: 3, -7, 120");
    expectOne("codes", "3", "/codes", "expected an array of 0 to 2 items, got '3'");
    expectOne("pair", R"([{"x":1,"label":"point"},{"x":10,"label":"point"}])"_json, "/pair/1/x",
              "expected an integer from 0 to 9, got 10");
    expectOne("empty", R"({"a":1})"_json, "/empty/a", "unexpected key 'a'; the object must be empty");
    expectOne("empty", nlohmann::json::array(), "/empty", "expected an empty object");
    expectOne("mode", R"({"state":"on"})"_json, "/mode/level", "missing key 'level'");
    expectOne("mode", R"({"state":"dim"})"_json, "/mode/state", R"(unknown state 'dim'; expected one of: "on", "off")");

    // A key with "/" and "~" is escaped in the path as RFC 6901 says.
    const nlohmann::json escaped = R"({"type": "object", "properties": {"a/b~c": {"const": 1}}, "required": ["a/b~c"]})"_json;
    const grammar::ValidationError odd = OnlyError(R"({"a/b~c": 2})"_json, escaped);
    CHECK(odd.path == "/a~1b~0c");
    CHECK(odd.message == "expected 1, got 2");
}

TEST_CASE("Validate throws the schema's DataError and never one for the value", "[schema]")
{
    CHECK_THROWS_AS(grammar::Validate(nlohmann::json::object(), R"({"type": "boolean"})"_json), core::DataError);
    // A value of any shape is data: errors, not exceptions.
    const nlohmann::json schema = test::CatalogSchema();
    const std::vector<nlohmann::json> values = {nlohmann::json(), nlohmann::json(1.5), nlohmann::json::array({1, 2}),
                                                R"({"action": null})"_json, R"({"action": "attack", "params": []})"_json};
    for (const nlohmann::json& value : values) {
        INFO(value.dump());
        std::vector<grammar::ValidationError> errors;
        CHECK_NOTHROW(errors = grammar::Validate(value, schema));
        CHECK_FALSE(errors.empty());
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors in `tests/grammar/SchemaTests.cpp`: `'Validate': is not a member of 'morpheus::grammar'` and `'ValidationError': is not a member of 'morpheus::grammar'`.

- [ ] **Step 4: Declare `Validate`**

In `src/grammar/Schema.h`, replace

```cpp
#include <string>

#include <nlohmann/json.hpp>
```

with

```cpp
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
```

and replace

```cpp
std::string FromSchema(const nlohmann::json& schema);
```

with

```cpp
std::string FromSchema(const nlohmann::json& schema);

// One mistake Validate found in a value.
struct ValidationError {
    std::string path = {};    // JSON pointer into the value, "" for the value itself.
    std::string message = {}; // What is wrong, naming the expected shape.
};

// Checks `value` against `schema`: exactly what FromSchema's grammar allows. Types (an integer is a JSON integer, not
// a float), enum and const, integer ranges, string lengths in characters (core::Utf8Length), required keys present
// and no extra keys, array sizes and each item. For a oneOf the discriminator's value picks the branch: a missing
// discriminator, or a value naming no branch, is one error at the discriminator's path; otherwise the errors are the
// chosen branch's, with full paths. Every error is reported, in document order: an object's keys in the schema's
// order (the order the grammar writes them), each with its nested errors, then the keys the schema does not allow.
// An empty result means the value is valid. Never throws on the value: a mistake in the value is data. A schema
// outside the subset throws the same DataError as FromSchema.
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema);
```

- [ ] **Step 5: Implement it**

In `src/grammar/Schema.cpp`, replace

```cpp
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "grammar/Gbnf.h"
```

with

```cpp
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "core/Text.h"
#include "grammar/Gbnf.h"
```

replace

```cpp
    throw core::ContractError("FromSchema: unhandled schema kind");
}

} // namespace
```

with

```cpp
    throw core::ContractError("FromSchema: unhandled schema kind");
}

// Adds to `errors` every way `value`, found at `path` inside the whole value, breaks `node`, in document order: an
// object's keys in the schema's order, then the keys it does not allow. Recursive, like Compile.
void CheckValue(const Node& node, const nlohmann::json& value, const std::string& path,
                std::vector<ValidationError>& errors)
{
    auto add = [&](const std::string& at, std::string message) { errors.push_back({at, std::move(message)}); };
    // A value as messages show it: a string in single quotes, anything else as compact JSON, both cut short.
    auto shown = [](const nlohmann::json& shownValue) {
        if (shownValue.is_string()) {
            const std::string& text = shownValue.get_ref<const std::string&>();
            const std::string cut = core::CutUtf8(text, core::SHORT_JSON_CHARS);
            return "'" + cut + (cut.size() < text.size() ? "...'" : "'");
        }
        // The replace handler keeps a string that is not UTF-8 from throwing: the value is data.
        std::string text = shownValue.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
        if (text.size() > core::SHORT_JSON_CHARS) {
            text.resize(core::SHORT_JSON_CHARS);
            text += "...";
        }
        return text;
    };
    // The allowed values as messages list them: JSON, comma-separated.
    auto listed = [](const std::vector<nlohmann::json>& listedValues) {
        std::string text;
        for (const nlohmann::json& listedValue : listedValues) {
            text += (text.empty() ? "" : ", ") + listedValue.dump();
        }
        return text;
    };
    // Equal as the grammar sees it: a string to a string, an integer to an integer (1.0 is not 1).
    auto same = [](const nlohmann::json& a, const nlohmann::json& b) {
        return (a.is_string() && b.is_string() && a == b) || (a.is_number_integer() && b.is_number_integer() && a == b);
    };

    switch (node.kind) {
    case Node::Kind::Object: {
        std::string keyList;
        for (const std::string& key : node.keys) {
            keyList += (keyList.empty() ? "'" : ", '") + key + "'";
        }
        if (!value.is_object()) {
            add(path, node.keys.empty() ? "expected an empty object" : "expected an object with the keys " + keyList);
            break;
        }
        // The schema's keys first, in its order; each present one is checked against its schema.
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const std::string at = path + "/" + PointerToken(node.keys[i]);
            if (!value.contains(node.keys[i])) {
                add(at, std::format("missing key '{}'", node.keys[i]));
            } else {
                CheckValue(node.children[i], value.at(node.keys[i]), at, errors);
            }
        }
        // Then every key the schema does not allow.
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (std::find(node.keys.begin(), node.keys.end(), it.key()) == node.keys.end()) {
                add(path + "/" + PointerToken(it.key()),
                    std::format("unexpected key '{}'; {}", it.key(),
                                node.keys.empty() ? "the object must be empty" : "the keys are " + keyList));
            }
        }
        break;
    }
    case Node::Kind::Enum: {
        const bool allowed = std::any_of(node.values.begin(), node.values.end(),
                                         [&](const nlohmann::json& allowedValue) { return same(allowedValue, value); });
        if (!allowed) {
            add(path, std::format("{} is not one of: {}", shown(value), listed(node.values)));
        }
        break;
    }
    case Node::Kind::Const:
        if (!same(node.values[0], value)) {
            add(path, std::format("expected {}, got {}", node.values[0].dump(), shown(value)));
        }
        break;
    case Node::Kind::Integer:
        // JsonIntInRange refuses floats and bools, and unsigned values beyond long long.
        if (!core::JsonIntInRange(value, node.minimum, node.maximum)) {
            add(path, std::format("expected an integer from {} to {}, got {}", node.minimum, node.maximum, shown(value)));
        }
        break;
    case Node::Kind::String: {
        if (!value.is_string()) {
            const std::string lengths = node.minCount == 0 ? std::format("at most {}", node.maxCount)
                                                           : std::format("{} to {}", node.minCount, node.maxCount);
            add(path, std::format("expected a string of {} characters, got {}", lengths, shown(value)));
            break;
        }
        // Lengths count characters (code points), as the grammar's `char` does for compact JSON text.
        const std::size_t length = core::Utf8Length(value.get_ref<const std::string&>());
        if (length > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("longer than {} characters", node.maxCount));
        } else if (length < static_cast<std::size_t>(node.minCount)) {
            add(path, std::format("shorter than {} characters", node.minCount));
        }
        break;
    }
    case Node::Kind::Array: {
        if (!value.is_array()) {
            add(path, std::format("expected an array of {} to {} items, got {}", node.minCount, node.maxCount, shown(value)));
            break;
        }
        if (value.size() < static_cast<std::size_t>(node.minCount) || value.size() > static_cast<std::size_t>(node.maxCount)) {
            add(path, std::format("has {} items; expected {} to {}", value.size(), node.minCount, node.maxCount));
        }
        // Every item is checked, even past the limit.
        for (std::size_t i = 0; i < value.size(); ++i) {
            CheckValue(node.children[0], value[i], std::format("{}/{}", path, i), errors);
        }
        break;
    }
    case Node::Kind::OneOf: {
        const std::string& key = node.children[0].keys[0];
        std::vector<nlohmann::json> tags;
        for (const Node& branch : node.children) {
            tags.push_back(branch.children[0].values[0]);
        }
        if (!value.is_object()) {
            add(path, std::format("expected an object whose '{}' is one of: {}", key, listed(tags)));
            break;
        }
        const std::string at = path + "/" + PointerToken(key);
        if (!value.contains(key)) {
            add(at, std::format("missing key '{}'", key));
            break;
        }
        // The discriminator picks the branch; the value is then checked against that branch alone.
        const nlohmann::json& tag = value.at(key);
        const auto chosen = std::find_if(node.children.begin(), node.children.end(), [&](const Node& candidate) {
            return same(candidate.children[0].values[0], tag);
        });
        if (chosen == node.children.end()) {
            add(at, std::format("unknown {} {}; expected one of: {}", key, shown(tag), listed(tags)));
            break;
        }
        CheckValue(*chosen, value, path, errors);
        break;
    }
    }
}

} // namespace
```

and append, after `FromSchema`'s definition and before the closing `} // namespace morpheus::grammar`:

```cpp
std::vector<ValidationError> Validate(const nlohmann::json& value, const nlohmann::json& schema)
{
    // A schema outside the subset is the host's data error, the same one FromSchema throws.
    const Node root = Compile(schema, "");
    std::vector<ValidationError> errors;
    CheckValue(root, value, "", errors);
    return errors;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[schema]"`
Expected: builds with no warnings; `All tests passed` (19 test cases: Task 6's 13, each schema error now also checked against `Validate`, and these 6).

- [ ] **Step 7: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 354`.

- [ ] **Step 8: Commit**

```bash
git add src/grammar/Schema.h src/grammar/Schema.cpp tests/grammar/SchemaTests.cpp
git commit -m "Add Validate: a value checked against the schema subset, mistakes as data" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 8: The property test in the fast suite

`RandomValue` draws values a schema allows, with a fixed seed. In the fast suite, 1000 values from the catalog schema (and 1000 from the every-keyword schema) pass `Validate`, and the matcher accepts each one's compact text against `FromSchema`'s grammar. A third test checks that the draws really reach every action, every size and the range ends.

**Files:**
- Create: `tests/grammar/RandomValue.h`
- Test: `tests/grammar/PropertyTests.cpp`

**Interfaces:**
- Consumes: `grammar::FromSchema`, `grammar::Validate` (Tasks 6, 7); `test::CatalogSchema`, `test::EveryKeywordSchema`; `test::GbnfMatcher`; `core::Utf8Length`.
- Produces (header `grammar/RandomValue.h`, namespace `morpheus::test`): `constexpr std::uint64_t PROPERTY_SEED = 20261114;`, `constexpr int PROPERTY_DRAWS = 1000;`, `long long RandomBetween(std::mt19937_64& random, long long low, long long high)`, `nlohmann::ordered_json RandomValue(const nlohmann::json& schema, std::mt19937_64& random)`. Task 9 uses `PROPERTY_SEED`, `PROPERTY_DRAWS` and `RandomValue`.

- [ ] **Step 1: Write the failing tests**

Create `tests/grammar/PropertyTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Text.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/RandomValue.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"

using namespace morpheus;

namespace {

// Draws test::PROPERTY_DRAWS values from `schema` with the fixed seed and checks that each, read back from its compact
// text as a host reads the model's answer, passes Validate, and that the text fits the schema's grammar.
void CheckRandomValues(const nlohmann::json& schema)
{
    const test::GbnfMatcher matcher(grammar::FromSchema(schema));
    std::mt19937_64 random(test::PROPERTY_SEED);
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const std::string text = test::RandomValue(schema, random).dump();
        INFO("value " << i << ": " << text);
        const std::vector<grammar::ValidationError> errors = grammar::Validate(nlohmann::json::parse(text), schema);
        for (const grammar::ValidationError& error : errors) {
            UNSCOPED_INFO(error.path << ": " << error.message);
        }
        CHECK(errors.empty());
        CHECK(matcher.Accepts(text));
    }
}

} // namespace

TEST_CASE("Random catalog actions pass Validate and the catalog grammar", "[property]")
{
    CheckRandomValues(test::CatalogSchema());
}

TEST_CASE("Random values of every keyword pass Validate and their grammar", "[property]")
{
    CheckRandomValues(test::EveryKeywordSchema());
}

TEST_CASE("RandomValue reaches every action, size and range end the schemas allow", "[property]")
{
    // The same draws as the property tests: the same seed, the same order.
    const nlohmann::json catalog = test::CatalogSchema();
    std::mt19937_64 random(test::PROPERTY_SEED);
    std::set<std::string> actions;
    std::set<std::size_t> itemLengths;
    bool nonAscii = false;
    bool escaped = false;
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const nlohmann::ordered_json value = test::RandomValue(catalog, random);
        actions.insert(value.at("action").get<std::string>());
        if (value.at("action") == "give_item") {
            const std::string item = value.at("params").at("item").get<std::string>();
            itemLengths.insert(core::Utf8Length(item));
            nonAscii = nonAscii || std::any_of(item.begin(), item.end(), [](char c) {
                           return (static_cast<unsigned char>(c) & 0x80) != 0;
                       });
            escaped = escaped || item.find_first_of("\"\\\n\t") != std::string::npos;
        }
    }
    CHECK(actions == std::set<std::string>{"none", "follow", "wait", "attack", "give_item", "end_conversation"});
    CHECK(itemLengths.contains(0));
    CHECK(itemLengths.contains(48));
    CHECK(nonAscii);
    CHECK(escaped);

    const nlohmann::json every = test::EveryKeywordSchema();
    std::mt19937_64 everyRandom(test::PROPERTY_SEED);
    std::set<std::size_t> tagSizes;
    std::set<std::size_t> codeSizes;
    std::set<std::size_t> nameLengths;
    std::set<long long> counts;
    std::set<long long> bigs;
    std::set<std::string> modes;
    for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
        const nlohmann::ordered_json value = test::RandomValue(every, everyRandom);
        tagSizes.insert(value.at("tags").size());
        codeSizes.insert(value.at("codes").size());
        nameLengths.insert(core::Utf8Length(value.at("name").get<std::string>()));
        counts.insert(value.at("count").get<long long>());
        bigs.insert(value.at("big").get<long long>());
        modes.insert(value.at("mode").at("state").get<std::string>());
    }
    CHECK(tagSizes == std::set<std::size_t>{1, 2, 3});
    CHECK(codeSizes == std::set<std::size_t>{0, 1, 2});
    CHECK(nameLengths == std::set<std::size_t>{2, 3, 4, 5, 6});
    CHECK(counts.contains(-20));
    CHECK(counts.contains(120));
    CHECK(bigs.contains(std::numeric_limits<long long>::min()));
    CHECK(bigs.contains(std::numeric_limits<long long>::max()));
    CHECK(modes == std::set<std::string>{"on", "off"});
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: `fatal error C1083: Cannot open include file: 'grammar/RandomValue.h': No such file or directory` in `tests/grammar/PropertyTests.cpp`.

- [ ] **Step 3: Write the helper**

Create `tests/grammar/RandomValue.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace morpheus::test {

// The property tests' fixed seed and number of draws, shared by the fast suite and the [llama] suite, so that both
// check the same values.
constexpr std::uint64_t PROPERTY_SEED = 20261114;
constexpr int PROPERTY_DRAWS = 1000;

// A number from low to high: each end a quarter of the time, otherwise uniform.
inline long long RandomBetween(std::mt19937_64& random, long long low, long long high)
{
    switch (std::uniform_int_distribution<int>(0, 3)(random)) {
    case 0:
        return low;
    case 1:
        return high;
    default:
        return std::uniform_int_distribution<long long>(low, high)(random);
    }
}

// A random value that `schema` allows (grammar::FromSchema's subset; the schema must be valid). Objects keep their
// keys in `required` order, so dump() writes the compact text the grammar expects. Strings mix printable ASCII, the
// characters JSON escapes (quote, backslash, control characters) and characters of two, three and four UTF-8 bytes;
// never U+007F, which the grammar's `char` takes only escaped while dump() writes it raw. Integers, string lengths and
// array sizes are drawn with RandomBetween, so the ends come up often and every value between them can.
inline nlohmann::ordered_json RandomValue(const nlohmann::json& schema, std::mt19937_64& random)
{
    // The characters strings are made of besides printable ASCII, as UTF-8.
    static const std::vector<std::string> SPECIAL_CHARACTERS = {
        "\"", "\\", "\n", "\t", "\r", "\b", "\f", "\x01", "\x1F", // dump() escapes these
        "\xC2\x80", "\xC3\xA9",                                   // U+0080, U+00E9: two bytes
        "\xE2\x82\xAC", "\xE6\x97\xA5",                           // U+20AC, U+65E5: three bytes
        "\xF0\x9F\x98\x80",                                       // U+1F600: four bytes
    };
    // An index from 0 to count - 1.
    auto pick = [&](std::size_t count) { return std::uniform_int_distribution<std::size_t>(0, count - 1)(random); };

    if (schema.contains("oneOf")) {
        const nlohmann::json& branches = schema.at("oneOf");
        return RandomValue(branches[pick(branches.size())], random);
    }
    if (schema.contains("enum")) {
        const nlohmann::json& values = schema.at("enum");
        return nlohmann::ordered_json(values[pick(values.size())]);
    }
    if (schema.contains("const")) {
        return nlohmann::ordered_json(schema.at("const"));
    }
    const std::string type = schema.at("type").get<std::string>();
    if (type == "integer") {
        return RandomBetween(random, schema.at("minimum").get<long long>(), schema.at("maximum").get<long long>());
    }
    if (type == "string") {
        const long long length = RandomBetween(random, schema.value("minLength", 0LL), schema.at("maxLength").get<long long>());
        std::string text;
        for (long long i = 0; i < length; ++i) {
            // Half the characters printable ASCII (DEL is not printable), half from the special list.
            if (std::uniform_int_distribution<int>(0, 1)(random) == 0) {
                text += static_cast<char>(std::uniform_int_distribution<int>(0x20, 0x7E)(random));
            } else {
                text += SPECIAL_CHARACTERS[pick(SPECIAL_CHARACTERS.size())];
            }
        }
        return text;
    }
    if (type == "array") {
        const long long size = RandomBetween(random, schema.value("minItems", 0LL), schema.at("maxItems").get<long long>());
        nlohmann::ordered_json array = nlohmann::ordered_json::array();
        for (long long i = 0; i < size; ++i) {
            array.push_back(RandomValue(schema.at("items"), random));
        }
        return array;
    }
    // An object: every key, in `required` order.
    nlohmann::ordered_json object = nlohmann::ordered_json::object();
    for (const nlohmann::json& key : schema.at("required")) {
        const std::string name = key.get<std::string>();
        object[name] = RandomValue(schema.at("properties").at(name), random);
    }
    return object;
}

} // namespace morpheus::test
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[property]"`
Expected: builds with no warnings; `All tests passed` (3 test cases). If the coverage test fails (for example no 48-character item came up), the draws do not exercise what the spec asks: change the helper's distribution, not the seed.

- [ ] **Step 5: Run the whole suite**

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 357`.

- [ ] **Step 6: Commit**

```bash
git add tests/grammar/RandomValue.h tests/grammar/PropertyTests.cpp
git commit -m "Add the schema property test: random values pass Validate and the grammar" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 9: The model-backed grammar tests

The same 1000 values (same seed, same draws) are checked by llama.cpp's grammar sampler through `LlamaCppBackend::CheckGrammar`; each `FromSchema` grammar for the test schemas parses; hand-made invalid actions are rejected; the rebuilt review grammars parse; and llama.cpp agrees with the test matcher on `IntRange` grammars. These tests exercise code that already exists, so they pass when first run; a failure is a real disagreement between the library and llama.cpp and must be investigated (superpowers:systematic-debugging), not papered over.

**Files:**
- Modify: `tests/llama/LlamaGrammarTests.cpp`

**Interfaces:**
- Consumes: `test::SharedBackend()` (`tests/llama/LlamaTestModels.h`); `LlamaCppBackend::CheckGrammar(grammar, text)` ("" when the text fits; a reason otherwise; `ContractError` for a grammar that does not parse); `grammar::FromSchema`, `grammar::GrammarBuilder`, `grammar::IntRange`; `turn::BuildReviewGrammar`; `test::RandomValue`, `test::PROPERTY_SEED`, `test::PROPERTY_DRAWS`, `test::CatalogSchema`, `test::EveryKeywordSchema`, `test::GbnfMatcher`.
- Produces: four `[llama]` test cases whose names start with `llama.cpp`.

- [ ] **Step 1: Write the tests**

In `tests/llama/LlamaGrammarTests.cpp`, replace

```cpp
#include <algorithm>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"
```

with

```cpp
#include <algorithm>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "LlamaTestModels.h"
#include "TestPaths.h"
#include "context/StateView.h"
#include "context/WorldReference.h"
#include "core/Changes.h"
#include "core/Errors.h"
#include "core/World.h"
#include "grammar/Gbnf.h"
#include "grammar/GbnfMatcher.h"
#include "grammar/RandomValue.h"
#include "grammar/Schema.h"
#include "grammar/TestSchemas.h"
#include "inference/Llm.h"
#include "turn/ResolveGrammar.h"
```

and append:

```cpp
TEST_CASE("llama.cpp accepts the random values of each test schema against the schema's grammar", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<nlohmann::json> schemas = {test::CatalogSchema(), test::EveryKeywordSchema()};
    for (const nlohmann::json& schema : schemas) {
        const std::string gbnf = grammar::FromSchema(schema);
        // The grammar parses (one that does not is a ContractError) and wants a whole value.
        CHECK(backend.CheckGrammar(gbnf, "") == "the text ends before the grammar does");
        // The same values as the fast suite's property test: the same seed, the same draws, written compact.
        std::mt19937_64 random(test::PROPERTY_SEED);
        for (int i = 0; i < test::PROPERTY_DRAWS; ++i) {
            const std::string text = test::RandomValue(schema, random).dump();
            INFO("value " << i << ": " << text);
            CHECK(backend.CheckGrammar(gbnf, text) == "");
        }
    }
}

TEST_CASE("llama.cpp rejects invalid actions against the catalog grammar", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::string gbnf = grammar::FromSchema(test::CatalogSchema());
    const std::vector<std::string> invalid = {
        R"({"action":"dance","params":{}})",                                             // an unknown action
        R"({"action":"attack","params":{"target":"0001C3D2"}})",                         // an actor who is not present
        R"({"action":"attack","params":{}})",                                            // a missing argument
        R"({"action":"wait","params":{},"intent":"rest"})",                              // an extra key
        R"({"action":"give_item","params":{"item":")" + std::string(49, 'a') + R"("}})", // an item over 48 characters
        R"({"action": "wait","params":{}})",                                             // whitespace
    };
    for (const std::string& text : invalid) {
        INFO(text);
        CHECK(backend.CheckGrammar(gbnf, text) != "");
    }
}

TEST_CASE("llama.cpp parses the review grammars built on the builder", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    // A grammar llama.cpp cannot parse is a ContractError, so each check also proves the grammar compiles.
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true),
                               R"({"consistent":false,"problems":["The roll failed."],"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, false), R"({"consistent":true,"problems":[]})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(false, true), R"({"digest":"A climb."})") == "");
    CHECK(backend.CheckGrammar(turn::BuildReviewGrammar(true, true), R"({"consistent":true,"problems":["x"],"digest":""})") != "");
}

TEST_CASE("llama.cpp and the test matcher agree on IntRange grammars", "[llama]")
{
    llama::LlamaCppBackend& backend = test::SharedBackend();
    const std::vector<std::pair<long long, long long>> ranges = {{-15, 123}, {0, 255}, {-1000, -91}, {37, 120}};
    for (const auto& [low, high] : ranges) {
        const std::string gbnf = grammar::GrammarBuilder().Text(grammar::IntRange(low, high));
        const test::GbnfMatcher matcher(gbnf);
        // Other spellings, then a window around each end.
        std::vector<std::pair<std::string, bool>> probes = {{"-0", false}, {"00", false}, {"007", false}, {"-01", false},
                                                            {"+5", false}};
        for (long long value = low - 12; value <= low + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (long long value = high - 12; value <= high + 12; ++value) {
            probes.emplace_back(std::to_string(value), low <= value && value <= high);
        }
        for (const auto& [text, inRange] : probes) {
            INFO("IntRange(" << low << ", " << high << ") on '" << text << "'");
            CHECK(backend.CheckGrammar(gbnf, text).empty() == inRange);
            CHECK(matcher.Accepts(text) == inRange);
        }
    }
}
```

- [ ] **Step 2: Build the dev tree**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`
Expected: builds with no warnings.

- [ ] **Step 3: Run the model-backed grammar tests**

Run (Git Bash, from the repo root; the variables are the README's):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"
```

Expected: `All tests passed` (the existing "llama.cpp accepts the generated grammars and they constrain real output", which now builds the resolve and lookup grammars with the builder, the existing `CheckGrammar` test, the existing "llama.cpp log lines reach the log sink", and these four). If any test reports SKIPPED, a variable is unset: set it and run again; a skip does not count as done.

- [ ] **Step 4: Commit**

```bash
git add tests/llama/LlamaGrammarTests.cpp
git commit -m "Check schema grammars, the review grammars and IntRange with llama.cpp" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Documentation and the full verification

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `README.md`, `CLAUDE.md`, `cmake/consumer-check/CMakeLists.txt`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation only.

- [ ] **Step 1: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, replace

```markdown
| `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
```

with

```markdown
| `context` | Layered prompt assembly, exact token budgets, cache-prefix tracking | core, inference |
| `grammar` | Bounded-JSON grammars for llama.cpp: the GBNF builder (`GrammarBuilder` and its expressions), `FromSchema` for a subset of JSON Schema, and `Validate`, which checks a value against the same subset. No llama.cpp | core |
```

replace

```markdown
**Build:** CMake, with dependencies pulled through FetchContent
```

with

```markdown
**Grammars:** `grammar` turns a bounded JSON shape into GBNF for llama.cpp (design: `docs/superpowers/specs/2026-11-14-grammar-library-design.md`). Its builder writes compact JSON with no optional whitespace: literals, bounded JSON strings, exact integer ranges (`IntRange`, any `long long` range, no leading zeros or `-0`), enums, bounded lists, objects with fixed keys in a fixed order and alternatives; `GrammarBuilder` collects them into named rules and writes the root line first, because `turn::CallKindOf` recognises a lookup and a review call by their grammar's first line. `FromSchema` covers a subset of JSON Schema: objects whose keys are all required (written in `required` order, no extra keys), `enum` and `const` of strings or integers, bounded integers, strings and arrays, and `oneOf` with a discriminator key; anything else is a `DataError` naming the JSON pointer inside the schema. `Validate` checks a value against the same subset and returns its mistakes as data, a JSON pointer into the value and a message each. Morpheus's own resolve, lookup and review grammars (`turn`) are built on the builder, not on JSON Schema.

**Build:** CMake, with dependencies pulled through FetchContent
```

replace

```markdown
it builds by default only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::turn`)
```

with

```markdown
it builds by default only the engine libraries (`Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar`, `Morpheus::turn`)
```

and replace

```markdown
- **`core` unit tests:** validation, change ops, dice. Grammar generation and its tests live in `turn` (`ResolveGrammarTests`).
```

with

```markdown
- **`core` unit tests:** validation, change ops, dice.
- **`grammar` tests:** the builder's text, `IntRange` checked over windows of many ranges and at the edges of `long long`, every rule of `FromSchema` and `Validate`, and a property test: 1000 random values drawn from a schema with a fixed seed pass `Validate` and the schema's grammar. The fast suite matches grammars with a test-only GBNF matcher (`tests/grammar/GbnfMatcher.h`); the `[llama]` suite checks the same values with llama.cpp's grammar sampler. Morpheus's grammars are tested in `turn` (`ResolveGrammarTests`), including what they accept and reject.
```

- [ ] **Step 2: The README**

In `README.md`, replace

```markdown
- `src/context`: layered prompt assembly with exact token budgets
```

with

```markdown
- `src/context`: layered prompt assembly with exact token budgets
- `src/grammar`: bounded-JSON grammars: the GBNF builder, `FromSchema` and `Validate` (no llama.cpp)
```

and replace

```markdown
Morpheus builds only its engine libraries, `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context` and `Morpheus::turn`,
```

with

```markdown
Morpheus builds only its engine libraries, `Morpheus::core`, `Morpheus::store`, `Morpheus::inference`, `Morpheus::context`, `Morpheus::grammar` and `Morpheus::turn`,
```

- [ ] **Step 3: CLAUDE.md**

In `CLAUDE.md`, replace

```markdown
- src/context: layered prompt assembly with exact token budgets.
```

with

```markdown
- src/context: layered prompt assembly with exact token budgets.
- src/grammar: bounded-JSON grammars: the GBNF builder, `FromSchema` and `Validate`. No llama.cpp.
```

- [ ] **Step 4: The consumer check's comment**

In `cmake/consumer-check/CMakeLists.txt`, replace

```cmake
# With Morpheus's vendored defaults nothing beyond the five engine libraries exists: no app, tests or tools, no
```

with

```cmake
# With Morpheus's vendored defaults nothing beyond the six engine libraries exists: no app, tests or tools, no
```

- [ ] **Step 5: Run the fast suite from a fresh configure**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: builds with no warnings; `100% tests passed, 0 tests failed out of 357`.

- [ ] **Step 6: Run the AddressSanitizer preset**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `100% tests passed, 0 tests failed out of 357`, with no AddressSanitizer report.

- [ ] **Step 7: Build every dev target and run the model-backed grammar tests**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run (Git Bash, from the repo root):

```bash
export MORPHEUS_TEST_CHAT_MODEL="$PWD/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="$PWD/models/nomic-embed-text-v1.5.Q8_0.gguf"
./build/dev/bin/RelWithDebInfo/morpheus_tests.exe "llama.cpp*" "LlamaCppBackend::CheckGrammar*"
```

Expected: `All tests passed`, none skipped.

- [ ] **Step 8: Check the vendored build**

Run: `cmake -S cmake/consumer-check -B build/consumer-check && cmake --build build/consumer-check --config Debug && ./build/consumer-check/Debug/consumer_check.exe`
Expected: configures (no unwanted target defined), builds, and prints `consumer_check: ok (rolled ..., total ...)`.

- [ ] **Step 9: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md CLAUDE.md cmake/consumer-check/CMakeLists.txt
git commit -m "Document the grammar library" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| New library `morpheus_grammar` in `src/grammar/`, namespace `morpheus::grammar`, alias `Morpheus::grammar`, `morpheus_target_defaults` | 3 |
| Depends on `morpheus_core` and nlohmann/json only; `morpheus_turn` links it; beside `context` in the order | 3, 5 |
| Architecture spec module table row and paragraph | 10 |
| `Literal` (escapes `"` and `\`) | 3 |
| `JsonString(minChars, maxChars)`, the `char` terminal as today, `0 <= minChars <= maxChars` | 3 |
| `IntRange(min, max)` exact for any `long long` range, no leading zeros, `-0` refused | 4 (algorithm, texts, brute force, whole range), 9 (llama.cpp agrees) |
| `Enum` (JSON strings, at least one, no duplicates) | 3 |
| `ListOf` (compact, `0 <= minItems <= maxItems`, `maxItems >= 1`) | 3 |
| `Object` (exact keys, given order, compact, at least one field, no duplicate keys) | 3 |
| `Alternatives` (at least one) | 3 |
| Broken contracts are `ContractError` | 3, 4 |
| `GrammarBuilder::Rule` (returns the name, duplicate is `ContractError`, names `[a-z0-9-]+`) | 3 |
| `GrammarBuilder::Text` (root line first, rules in order, shared terminals used) | 3 (only `char` exists; see Choices) |
| Compact output, no optional whitespace | 3 (matcher tests refuse whitespace), 2, 6 |
| Escaping: Literal for GBNF, JSON-escaped values inside JSON strings | 3 (`Enum`, `Object` keys), 5 (held item names) |
| Morpheus's three grammars rewritten with the builder | 5 |
| Same language, including the counter path, nonzero `add`, the review's verdict/problems tie | 2 (characterization), 5 (kept green), 9 (existing `[llama]` test) |
| `int` is `IntRange(-999999999, 999999999)`, `pos-int` is `IntRange(1, 999999999)`, nonzero as the union | 4 (texts), 5 |
| First lines unchanged; `CallKindOf` and replays keep working | 5 (Steps 1, 7; `ReplayTests`) |
| Accept/reject and model-backed tests stay as they are and pass; exact-text tests updated and say so | 5 (Step 1 comments), 9 |
| Private helpers (`Terminals`, `Literal`, `Alternatives`, `ListOf`) removed, no copies | 5 (Step 8) |
| `FromSchema` for exactly the subset table | 6 |
| Ignored keywords `title`, `description`, `$comment`, `$schema`, `$id` | 6 |
| Any other keyword, unsupported type, missing bound, broken rule is a `DataError` naming the JSON pointer | 6 (one test per rule, nested pointer test) |
| Nested schemas at any depth; `$ref` not supported | 6 |
| `ValidationError {path, message}`; `Validate` signature; empty result means valid | 7 |
| Checks exactly what the grammar allows (types, enum/const, ranges, lengths in characters, keys, arrays) | 7 |
| `oneOf` by discriminator; missing or unknown discriminator is one error at its path | 7 |
| Every error, in document order | 7 |
| Never throws on the value; a bad schema throws `FromSchema`'s `DataError` | 7 (SchemaError helper compares both; throws test) |
| Messages name the expected shape | 7 |
| Builder tests: text of each function, `IntRange` edge cases, the `ContractError`s | 3, 4 |
| Test schema equivalent to the host's example catalog | 6 (`CatalogSchema`) |
| Invalid values at the right path: unknown action, absent NPC, missing target, item over 48, extra key, float for integer | 7 |
| A `DataError` test per schema rule naming the pointer | 6 |
| `tests/grammar/RandomValue.h`: fixed seed, non-ASCII and escapes, range ends, every array size | 8 |
| Fast suite: 1000 catalog values pass `Validate` | 8 |
| `[llama]`: `CheckGrammar(FromSchema(schema), value.dump())` accepts the same 1000 values | 9 |
| `[llama]`: each `FromSchema` grammar for the test schemas parses | 9 |
| `[llama]`: hand-made invalid strings are rejected | 9 |
| Documentation: architecture spec, README layout, CLAUDE.md layout | 10 |
| Out of scope: catalog-to-schema conversion, observation validation, an explicit call tag | not touched |
