### Task 2: Turn-record types move into core

**Files:**
- Create: `src/core/TurnRecord.h`, `src/core/TurnRecord.cpp`, `tests/core/TurnRecordTests.cpp`
- Modify: `src/core/CMakeLists.txt` (add `TurnRecord.cpp`), `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `CheckResult` (`core/Dice.h`); `Change`, `AppliedChange`, `RejectedChange`, `ChangeToJson`, `ParseChange` (`core/Changes.h`); `JsonIntInRange` (`core/JsonInt.h`).
- Produces (namespace `morpheus::core`):
  - `struct CheckOutcome { std::string stat; std::string difficulty; std::string stakes; CheckResult result; }`
  - `struct TurnRecord { int number; std::string input; nlohmann::json resolve; std::vector<CheckOutcome> checks; std::vector<AppliedChange> applied; std::vector<RejectedChange> rejected; std::string narration; nlohmann::json timings; std::vector<std::string> notes; }`
  - `struct Summary { int fromTurn; int toTurn; std::string text; }`
  - `struct Note { int turn; std::string text; }`
  - `CheckOutcomeToJson` / `CheckOutcomeFromJson`, `AppliedChangeToJson` / `AppliedChangeFromJson`, `RejectedChangeToJson` / `RejectedChangeFromJson`. Each FromJson throws `DataError` on a bad shape.
- `store` re-exports the types (`using core::TurnRecord;` and so on), so `store::TurnRecord` keeps working.

- [ ] **Step 1: Write the failing test `tests/core/TurnRecordTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Errors.h"
#include "core/TurnRecord.h"

using namespace morpheus::core;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("CheckOutcome round-trips through JSON", "[turnrecord]")
{
    CheckOutcome outcome;
    outcome.stat = "agility";
    outcome.difficulty = "hard";
    outcome.stakes = "fall vs reach the hatch";
    outcome.result.rolls = {9, 5};
    outcome.result.statValue = 3;
    outcome.result.total = 17;
    outcome.result.target = 16;
    outcome.result.success = true;

    const CheckOutcome back = CheckOutcomeFromJson(CheckOutcomeToJson(outcome));
    CHECK(back.stat == "agility");
    CHECK(back.difficulty == "hard");
    CHECK(back.stakes == "fall vs reach the hatch");
    CHECK(back.result.rolls == std::vector<int>{9, 5});
    CHECK(back.result.statValue == 3);
    CHECK(back.result.total == 17);
    CHECK(back.result.target == 16);
    CHECK(back.result.success);
}

TEST_CASE("Applied and rejected changes round-trip through JSON", "[turnrecord]")
{
    const AppliedChange applied{Change{ChangeOp::Add, "player.health", -3, "fell"}, "clamped to 0"};
    const AppliedChange appliedBack = AppliedChangeFromJson(AppliedChangeToJson(applied));
    CHECK(appliedBack.change.op == ChangeOp::Add);
    CHECK(appliedBack.change.path == "player.health");
    CHECK(appliedBack.change.value == -3);
    CHECK(appliedBack.change.reason == "fell");
    CHECK(appliedBack.note == "clamped to 0");

    const RejectedChange rejected{nlohmann::json{{"op", "explode"}}, "unknown op"};
    const RejectedChange rejectedBack = RejectedChangeFromJson(RejectedChangeToJson(rejected));
    CHECK(rejectedBack.raw == nlohmann::json{{"op", "explode"}});
    CHECK(rejectedBack.reason == "unknown op");
}

TEST_CASE("Malformed stored turn data is a DataError", "[turnrecord]")
{
    CHECK_THROWS_AS(CheckOutcomeFromJson(nlohmann::json::array()), DataError);
    CHECK_THROWS_WITH(CheckOutcomeFromJson(nlohmann::json::parse(
                          R"({"stat":"a","difficulty":"b","stakes":"c","rolls":[7],"stat_value":0,"total":7,"target":8})")),
                      ContainsSubstring("success"));
    CHECK_THROWS_AS(AppliedChangeFromJson(nlohmann::json::parse(R"({"change":{"path":"x"},"note":""})")), DataError);
    CHECK_THROWS_AS(RejectedChangeFromJson(nlohmann::json::parse(R"({"raw":1})")), DataError);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/TurnRecord.h'`.

- [ ] **Step 3: Create `src/core/TurnRecord.h`**

```cpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Changes.h"
#include "core/Dice.h"

namespace morpheus::core {

// One resolved check: the stat and difficulty the model asked for, the stakes it named, and what the dice did.
struct CheckOutcome {
    std::string stat = {};
    std::string difficulty = {};
    std::string stakes = {};
    CheckResult result = {};
};

// Everything one committed turn produced. Stored by store::RunStore and rendered into the transcript (L3) by
// the context module.
struct TurnRecord {
    int number = 0;
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output exactly as the model produced it.
    std::vector<CheckOutcome> checks = {};
    std::vector<AppliedChange> applied = {};
    std::vector<RejectedChange> rejected = {};
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
    std::vector<std::string> notes = {}; // Model-written notes (assisted memory mode), committed with the turn.
};

// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
};

// A model-written memory from assisted memory mode. Notes exist only for committed turns.
struct Note {
    int turn = 0;
    std::string text = {};
};

// JSON forms used for storage. Each FromJson throws DataError naming the missing or malformed member.
nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome);
CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json);
nlohmann::json AppliedChangeToJson(const AppliedChange& applied);
AppliedChange AppliedChangeFromJson(const nlohmann::json& json);
nlohmann::json RejectedChangeToJson(const RejectedChange& rejected);
RejectedChange RejectedChangeFromJson(const nlohmann::json& json);

} // namespace morpheus::core
```

- [ ] **Step 4: Create `src/core/TurnRecord.cpp`, and add it to `src/core/CMakeLists.txt` after `JsonInt.cpp`**

```cpp
#include "core/TurnRecord.h"

#include <format>
#include <limits>
#include <optional>

#include "core/Errors.h"
#include "core/JsonInt.h"

namespace morpheus::core {

nlohmann::json CheckOutcomeToJson(const CheckOutcome& outcome)
{
    return {
        {"stat", outcome.stat},
        {"difficulty", outcome.difficulty},
        {"stakes", outcome.stakes},
        {"rolls", outcome.result.rolls},
        {"stat_value", outcome.result.statValue},
        {"total", outcome.result.total},
        {"target", outcome.result.target},
        {"success", outcome.result.success},
    };
}

CheckOutcome CheckOutcomeFromJson(const nlohmann::json& json)
{
    // The only writer is CheckOutcomeToJson, so any other shape means the stored data is damaged.
    auto malformed = [&](const char* member) {
        return DataError(std::format("check outcome: missing or malformed '{}' in {}", member, json.dump()));
    };
    if (!json.is_object()) {
        throw malformed("object");
    }
    auto requireString = [&](const char* key) {
        if (!json.contains(key) || !json.at(key).is_string()) {
            throw malformed(key);
        }
        return json.at(key).get<std::string>();
    };
    auto requireInt = [&](const char* key, long long lo, long long hi) {
        const std::optional<long long> value = json.contains(key) ? JsonIntInRange(json.at(key), lo, hi) : std::nullopt;
        if (!value) {
            throw malformed(key);
        }
        return *value;
    };

    CheckOutcome outcome;
    outcome.stat = requireString("stat");
    outcome.difficulty = requireString("difficulty");
    outcome.stakes = requireString("stakes");

    if (!json.contains("rolls") || !json.at("rolls").is_array()) {
        throw malformed("rolls");
    }
    for (const nlohmann::json& roll : json.at("rolls")) {
        const std::optional<long long> face = JsonIntInRange(roll, 1, 1000);
        if (!face) {
            throw malformed("rolls");
        }
        outcome.result.rolls.push_back(static_cast<int>(*face));
    }

    constexpr long long INT_LO = std::numeric_limits<int>::min();
    constexpr long long INT_HI = std::numeric_limits<int>::max();
    outcome.result.statValue = static_cast<int>(requireInt("stat_value", INT_LO, INT_HI));
    outcome.result.total = requireInt("total", std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());
    outcome.result.target = static_cast<int>(requireInt("target", INT_LO, INT_HI));

    if (!json.contains("success") || !json.at("success").is_boolean()) {
        throw malformed("success");
    }
    outcome.result.success = json.at("success").get<bool>();
    return outcome;
}

nlohmann::json AppliedChangeToJson(const AppliedChange& applied)
{
    return {{"change", ChangeToJson(applied.change)}, {"note", applied.note}};
}

AppliedChange AppliedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("change") || !json.contains("note") || !json.at("note").is_string()) {
        throw DataError(std::format("applied change: malformed {}", json.dump()));
    }
    // The stored change was valid when applied, so a parse failure means damaged data.
    std::string error;
    const std::optional<Change> change = ParseChange(json.at("change"), error);
    if (!change) {
        throw DataError(std::format("applied change: {}", error));
    }
    return {*change, json.at("note").get<std::string>()};
}

nlohmann::json RejectedChangeToJson(const RejectedChange& rejected)
{
    return {{"raw", rejected.raw}, {"reason", rejected.reason}};
}

RejectedChange RejectedChangeFromJson(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("raw") || !json.contains("reason") || !json.at("reason").is_string()) {
        throw DataError(std::format("rejected change: malformed {}", json.dump()));
    }
    return {json.at("raw"), json.at("reason").get<std::string>()};
}

} // namespace morpheus::core
```

- [ ] **Step 5: Switch `store` to the core types**

**`src/store/RunStore.h`:**
- Delete the `TurnRecord`, `Summary` and `Note` structs.
- Add `#include "core/TurnRecord.h"`.
- At the top of `namespace morpheus::store`, add:

```cpp
// Turn data types live in core; store re-exports them so callers can keep writing store::TurnRecord.
using core::Note;
using core::Summary;
using core::TurnRecord;
```

**`src/store/RunStore.cpp`:**

1. In `SCHEMA_SQL`, rename the `turns` column `rolls` to `checks`.

2. In `CommitTurn`, serialize the typed members, then bind them in place of the old JSON members. The column list becomes `(number, input, resolve, checks, applied, rejected, narration, timings)`:

```cpp
    // Typed turn data is stored as JSON arrays in its own columns.
    nlohmann::json checks = nlohmann::json::array();
    for (const core::CheckOutcome& check : record.checks) {
        checks.push_back(core::CheckOutcomeToJson(check));
    }
    nlohmann::json applied = nlohmann::json::array();
    for (const core::AppliedChange& change : record.applied) {
        applied.push_back(core::AppliedChangeToJson(change));
    }
    nlohmann::json rejected = nlohmann::json::array();
    for (const core::RejectedChange& change : record.rejected) {
        rejected.push_back(core::RejectedChangeToJson(change));
    }
```

   Bind `checks.dump()`, `applied.dump()` and `rejected.dump()` to `?4`, `?5` and `?6`.

3. In `Turns`, select `checks` instead of `rolls`. After `parseColumn`, convert the three arrays. A JSON value that is not an array, or an element whose FromJson throws `core::DataError`, becomes a `StoreError` with the same wording as the parse error ("'<file>': turn N column '<name>' ..."). Use one local generic lambda for the three conversions:

```cpp
        // A stored column that parses but has the wrong shape is damaged data too.
        auto convertColumn = [&](int column, const char* name, auto fromJson, auto& out) {
            const nlohmann::json array = parseColumn(column, name);
            try {
                if (!array.is_array()) {
                    throw core::DataError("not an array");
                }
                for (const nlohmann::json& element : array) {
                    out.push_back(fromJson(element));
                }
            } catch (const core::DataError& error) {
                throw StoreError(std::format("'{}': turn {} column '{}' is malformed: {}",
                                             _file.generic_string(), record.number, name, error.what()));
            }
        };
        convertColumn(3, "checks", core::CheckOutcomeFromJson, record.checks);
        convertColumn(4, "applied", core::AppliedChangeFromJson, record.applied);
        convertColumn(5, "rejected", core::RejectedChangeFromJson, record.rejected);
```

- [ ] **Step 6: Migrate `tests/store/RunStoreTests.cpp`**

Replace the body of `TestTurn` with typed data, keeping its signature `TurnRecord TestTurn(int number, std::vector<std::string> notes = {})`:

```cpp
    TurnRecord record;
    record.number = number;
    record.input = std::format("input {}", number);
    record.notes = std::move(notes);
    record.resolve = {{"intent", "climb"}};
    morpheus::core::CheckOutcome check;
    check.stat = "agility";
    check.difficulty = "hard";
    check.stakes = "fall vs reach the hatch";
    check.result.rolls = {14};
    check.result.total = 14;
    check.result.target = 16;
    check.result.success = false;
    record.checks = {check};
    record.applied = {morpheus::core::AppliedChange{
        morpheus::core::Change{morpheus::core::ChangeOp::Set, "player.location", "hatch", ""}, ""}};
    record.narration = std::format("narration {}", number);
    record.timings = {{"resolve_ms", 120}};
    return record;
```

Update the assertions that read the old JSON members. The test currently has:
- `CHECK(first->rolls.at(0).at("total") == 14);`
- `CHECK(first->applied.size() == 1);`
- `CHECK(first->rejected.empty());`

They become:

```cpp
    REQUIRE(first->checks.size() == 1);
    CHECK(first->checks.at(0).stat == "agility");
    CHECK(first->checks.at(0).result.total == 14);
    REQUIRE(first->applied.size() == 1);
    CHECK(first->applied.at(0).change.path == "player.location");
    CHECK(first->rejected.empty());
```

Add a test for a stored column that parses but has the wrong shape:

```cpp
TEST_CASE("A turn column with the wrong shape is a StoreError naming the column", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_bad_checks");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        store.CommitTurn(TestTurn(1), StateWithHealth(9));
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE turns SET checks = '[{\"stat\":1}]' WHERE number = 1");
    }
    RunStore store = RunStore::Open(file);
    CHECK_THROWS_WITH(store.Turn(1), ContainsSubstring("column 'checks'"));
}
```

Any other test that used `TurnRecord`'s old JSON members gets the same mechanical change.

- [ ] **Step 7: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including `[turnrecord]`.

- [ ] **Step 8: Commit**

```bash
git add src/core src/store tests
git commit -m "Move typed turn records into core" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

