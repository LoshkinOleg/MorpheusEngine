# M1 hardening round: requirements

Context: milestone 1 (`core` + `store`) is done and passed a final whole-branch review. That review asked for the fixes below. They must land before milestone 2 builds on this code, and before any saved run exists. No runs exist yet, so changing data formats is free. `RunStore::SCHEMA_VERSION` stays 1.

**Rules that apply to every fix**
- Follow CLAUDE.md: fail fast; comments in method bodies; a helper only when it has more than one caller; class layout order; ALL_CAPS constants; ASCII only in comments.
- Follow the global constraints in `.superpowers/sdd/global-constraints.md`.
- Use TDD for each fix: write the test, show it failing, implement, show it passing.
- Make one commit per numbered section.
- End every commit message with `-m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"`.
- Never stage the untracked `dotnet/` folder.
- Build with `cmake --build --preset tests`. Run focused tests with `./build/tests/bin/Debug/morpheus_tests.exe "[tag]"`. Run `ctest --preset tests` before each commit.

---

## H1. A shared int-range helper (do this first; later sections use it)

The "is this JSON an integer within [lo, hi]" logic is copied in five places:
- `StateSchema.cpp`: the bounds check in `FromJson`, the `Int` value check and the counter-count check in `CheckFieldValue`.
- `Changes.cpp`: `IsIntRangeInteger`.
- `World.cpp`: the difficulty targets.

Create `src/core/JsonInt.h` and `src/core/JsonInt.cpp` (add the .cpp to `src/core/CMakeLists.txt`):

```cpp
// Returns the value when `value` is a JSON integer (not a float or bool) within [lo, hi], otherwise nullopt.
// Unsigned values are range-checked before conversion, so uint64 values above LLONG_MAX can never wrap.
std::optional<long long> JsonIntInRange(const nlohmann::json& value, long long lo, long long hi);
```

Replace all five sites with it. Keep each site's existing error or rejection messages, adapted only as needed.

Add `tests/core/JsonIntTests.cpp` (tag `[jsonint]`). It must cover:
- the exact lo and hi boundaries (accepted);
- lo-1 and hi+1 (rejected);
- INT_MIN and INT_MAX as bounds;
- uint64 max (rejected);
- 2.5, `true` and `"5"` (rejected);
- negative values.

Also add the missing negative-boundary tests the reviews asked for:
- `Changes`: `add player.health -2147483648` clamps to 0; `add player.inventory.rope -2147483648` removes rope.
- `StateSchema`: schema bounds of INT_MIN and INT_MAX are accepted.

## H2. Fail-fast basics

1. `MORPHEUS_REQUIRE` (`src/core/Errors.h`): append the source location to the message, in the form `message + " [" __FILE__ ":" + std::to_string(__LINE__) + "]"`.
2. `tests/TestPaths.h` `WriteFile`: after writing, close the stream. If it failed to open or write, throw `std::runtime_error` naming the path.
3. `src/core/Csv.cpp`: add `#include <cstddef>` and `#include <utility>`.
4. `src/store/Sqlite.cpp`: replace both `sqlite3_close` calls with `sqlite3_close_v2`.
5. `src/core/Changes.h` / `Changes.cpp`: document the precondition on `ApplyChanges`: "state must already satisfy schema.ValidateState". Enforce it at the points where the code reads entity shape:
   - the entity has a string "type" naming a known type;
   - the entity has a "fields" object;
   - the addressed field is present.
   
   Use `MORPHEUS_REQUIRE`, so that a mismatched state throws `ContractError` (an engine bug: callers must validate loaded state) instead of a raw nlohmann exception. Add a test: a state whose player is missing the `location` field, followed by `set player.location`, throws `ContractError`.

## H3. Unknown keys are errors

Typos must fail loudly. Reject any key outside the allowed set, and name the key in a `DataError`.
- **`state_schema.json` top level:** only `entity_types`.
- **Entity type object:** only `creatable` and `fields`.
- **Field object:** `type` and `default` always. Add `min` and `max` only when the type is `int`, and `values` only when the type is `enum`. So `min` on a string field is an error.
- **`world.json` top level:** only `id`, `title`, `dice`, `check_entity`, `stats` (see H4), `difficulties`, `memory_mode`.

Add tests (tags `[schema]` and `[world]`):
- `"creatible": true` on a type;
- `"defualt"` on a field;
- `"min"` on a string field;
- an unknown world.json key such as `"memory_mod"`.

Each must throw, with the message containing the key.

## H4. Explicit stats in world.json

The current rule makes every int field of `check_entity` a check stat, so sandcrawler's `health` counts as one.
- Add a required `"stats"` array to world.json: non-empty, unique, non-empty strings.
- Each entry must be an `int` field of the check entity's type. Look up the type of `initialState[check_entity]` in the schema. Otherwise throw `DataError` naming the stat.
- Store it as `std::vector<std::string> stats` in `World`, in declaration order, placed after `checkEntity`. Update the comment on `checkEntity` accordingly.

Data:
- `game_projects/sandcrawler/world.json`: `"stats": ["grit", "wits", "presence", "agility"]`.
- `tests/data/worlds/mini/world.json`: `"stats": ["agility"]`.
- Update the `MiniWorldJsonWith` helper's base JSON in `tests/core/WorldTests.cpp` to include `stats`.

Tests:
- the mini world's stats equal `{"agility"}`;
- the sandcrawler world's stats are the four above;
- an error for a stat that is not a field (`"luck"`);
- an error for a non-int stat (`"location"`);
- an error for an empty array.

## H5. World content hash: stable format, line endings normalized

1. In `LoadWorld`, normalize every file's text by converting CRLF to LF before hashing and parsing. Rules and lore must never carry `\r` into prompts. Do this inline in the read loop; it has one caller.
2. Change the hash format so file boundaries are unambiguous. For each file in the fixed order, add, in sequence:
   - the name;
   - `"\n"`;
   - the decimal byte length of the normalized text;
   - `"\n"`;
   - the text.
   
   Document the format in a comment.
3. Add a `.gitattributes` at the repo root:
   ```
   game_projects/** text eol=lf
   tests/data/** text eol=lf
   ```
   Then run `git add --renormalize game_projects tests/data` so committed files are LF.
4. Tests:
   - A world copied to temp with CRLF line endings, written via `WriteFile` with `\r\n`, hashes equal to the LF version.
   - `world.rules` contains no `\r`.
   - ContentHash: a high-bit byte. Use the reference value of FNV-1a 64 for the single byte 0xFF, `af64724c8602eb6e`; verify it independently with a quick computation, and if it differs, use the computed value and say so in your report.
   - ContentHash: `Add("")` leaves the value unchanged.

## H6. Dice: seed mixing, overflow-safe total, pinned outputs

1. `CheckSeed`: mix the run seed first, so runs whose seeds differ only in high bits cannot collide across turns.
   - `std::uint64_t base = SplitMix64(runSeed).Next();`
   - then `SplitMix64 mixer(base ^ ((uint64(turn) << 32) | uint64(checkIndex)));` and `return mixer.Next();`
   
   Keep the turn and checkIndex preconditions.
2. `CheckResult::total` becomes `long long`, and `RollCheck` accumulates in `long long`. Stats and targets are int-range, and dice add at most 20*1000, so no overflow is possible.
3. Tests (tag `[dice]`):
   - Pin golden values. Compute them once with the new code, then hard-code them:
     - `CheckSeed(42, 1, 0)`;
     - the rolls of `RollCheck(DiceRule::Parse("2d6+stat"), 3, 10, CheckSeed(42, 1, 0))`.
     
     Comment that they guard replay stability.
   - The success boundary: find or construct a case where total == target (success), and one where total == target - 1 (failure). Choose the target from a known roll.
   - A stat near INT_MAX: `RollCheck(1d20+stat, INT_MAX, ...)` gives `total > INT_MAX` with no UB.

## H7. Notes are committed with their turn

Model-written notes for turn N must not exist unless turn N commits.
- Add `std::vector<std::string> notes = {};` to `TurnRecord`.
- `CommitTurn` inserts each note into the `notes` table, with `turn = record.number`, inside the same transaction.
- `Turns()` and `Turn()` fill `record.notes` from the notes table, ordered by id.
- Remove the public `AddNote`. `Notes()` stays: it lists all notes for context assembly.
- Update the RunStore.h doc comments.
- Update the existing tests that used `AddNote` to pass notes through `CommitTurn` instead. This includes the TruncateAfter test.
- Add a test: a failed `CommitTurn` (wrong turn number) leaves no notes.

## H8. Embeddings carry a content hash

Edited lore must not silently keep a stale vector.
- Add `content_hash TEXT NOT NULL` to the `embeddings` table, in `SCHEMA_SQL`. The primary key stays `(kind, ref_id, model)`.
- Add `std::string contentHash = {};` to `EmbeddingRow`. Document it as the hash of the exact text that was embedded; callers re-embed when it differs from the current text's hash.
- `PutEmbedding` requires a non-empty contentHash (`MORPHEUS_REQUIRE`) and stores it. `Embeddings()` returns it.
- Update the existing embedding tests to set and check `contentHash`. Add a case where a replace changes the stored hash.

---

## Report

Write `.superpowers/sdd/hardening-report.md`, with one section per H-number. Each section gives:
- what changed;
- the RED and GREEN commands and their output;
- the commit SHA.

Finish with the full `ctest --preset tests` result.
