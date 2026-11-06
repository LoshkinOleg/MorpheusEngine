# Task 6 fix round: resolve grammar review findings

The owner decided the two plan-level items:
- **Held counter items:** allow their names in the grammar.
- **The model-backed grammar test:** strengthen it.

Fix all of the following in `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `tests/turn/ResolveGrammarTests.cpp` and `tests/llama/LlamaGrammarTests.cpp`. Use TDD: write each test first, show it failing, then implement.

## 1. Held counter items are reachable

The schema allows any counter item name without a '.', but `item ::= [a-z0-9_]{1,32}` cannot spell names such as "Rusty Knife" or "water-flask", so such held items can never be decremented or removed.

**Change:** for each counter field of each live entity, the path's item part is either:
- one of the items the entity holds now, written verbatim; or
- the generic `item` rule, for new items.

Build a per-field rule, e.g. `items-N ::= "<held 1>" | "<held 2>" | item`, or just `item` when nothing is held. Use it in that field's add and remove alternatives.

**Escaping:** the held name sits inside the JSON path string, so the literal must match the name's JSON-escaped form without the surrounding quotes. Take `nlohmann::json(name).dump()`, strip the first and last characters, then pass it through `Literal()`.

**Docs:** update the header comment to say that new items use `[a-z0-9_]{1,MAX_ITEM_CHARS}`, while held items are matched by name.

**Unit test:** a state whose counter holds "Rusty Knife" and `water-flask`. The grammar contains a literal for each, and still the generic `item` alternative.

## 2. Unit tests: GBNF structure and the untested branches

Add to `tests/turn/ResolveGrammarTests.cpp`, using an in-memory `StateSchema::FromJson` world, built the way existing tests build worlds, or by editing a loaded mini world's schema and state:
- **A structural check** in a test-local function. Every rule name referenced outside quotes and character classes is defined exactly once on the left of `::=`; parentheses balance; quotes balance. Apply it to the engine-mode grammar, the assisted-mode grammar, the lookup grammar, and the held-item grammar.
- A **list field** (add/remove) alternative, and an **enum `set` on an existing entity**.
- An **enum value containing `"` and `\`**, correctly escaped in its alternative.
- **Enum rule sharing:** a live entity's enum field and a create of the same type reference the same `enum-N` rule. Find the name by searching, not by assuming `enum-0`; also make the existing `enum-0` assertion robust the same way.
- The **list bound**: the changes list has `{0,5}` (MAX_CHANGES - 1).
- The **ContractError** for non-object state.

## 3. A world with nothing to change is a data condition

`MORPHEUS_REQUIRE(!changes.empty())` is not an engine bug: a world whose entities have no changeable fields and no creatable type is legal data. Instead, emit a grammar whose `changes` and `branch` lists can only be `"[]"`, with no `change` rule. Test it with a schema that has no fields and no creatable type, if the schema loader allows that; if the loader forbids empty `fields` (it requires non-empty fields), say so in the report and leave the REQUIRE with a comment that loaded worlds cannot reach it.

## 4. Minor tidy-ups

- Hoist the repeated `entity.value().at("type").get<std::string>()` into a local `typeName`.
- Align the trailing comments in the constants block.
- Add `static_assert(MAX_CHECKS >= 2 && ...)` for every constant passed to `ListOf`, next to the constants or in the .cpp. Keep the runtime REQUIRE.

## 5. Strengthen the model-backed test (`tests/llama/LlamaGrammarTests.cpp`)

- Raise `spec.maxTokens` to 1024, so a valid but long proposal cannot fail the test.
- After parsing, check:
  - every check's `stat` is in `world.stats`, and its `difficulty` is a key of `world.difficulties`;
  - the output's changes apply: run `core::ApplyChanges(world.schema, world.initialState, changes)` on the unconditional `changes` plus every check's `on_success` and `on_failure` entries. Every entry must parse, so `REQUIRE` that no rejection reason is a parse failure from `ParseChange`.
- Value rejections (out-of-range ints, unknown entity ids for creates) are legitimate engine rejections. Do not fail on them, but `WARN` them so they show in the report.
- Keep the speed `WARN` line.

## Verify

Run:
- `[grammar]` under `tests`;
- `ctest --preset tests`;
- `[llama]` under `dev`, with the model variables exported in the same command.

## Commit

One commit: "Match held counter items in the grammar and test the grammar's structure". Stage only the four files above.
