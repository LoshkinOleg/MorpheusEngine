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
