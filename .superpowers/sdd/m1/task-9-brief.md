### Task 9: Turns, snapshots and undo

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `RunStore`, `Database`, `Statement` (Task 8).
- Produces (namespace `morpheus::store`):
  - `struct TurnRecord { int number; std::string input; nlohmann::json resolve, rolls, applied, rejected; std::string narration; nlohmann::json timings; }`
  - `void RunStore::CommitTurn(const TurnRecord&, const nlohmann::json& stateAfter)`
  - `std::optional<TurnRecord> RunStore::Turn(int)`
  - `std::vector<TurnRecord> RunStore::Turns(int fromTurn, int toTurn)`
  - `void RunStore::TruncateAfter(int turn)`

- [ ] **Step 1: Append the failing tests to `tests/store/RunStoreTests.cpp`**

Add `#include <format>` to the includes at the top of the file. Then append:

```cpp
namespace {

TurnRecord TestTurn(int number)
{
    TurnRecord record;
    record.number = number;
    record.input = std::format("input {}", number);
    record.resolve = {{"intent", "climb"}};
    record.rolls = nlohmann::json::array({nlohmann::json{{"total", 14}}});
    record.applied = nlohmann::json::array({nlohmann::json{{"op", "set"}}});
    record.rejected = nlohmann::json::array();
    record.narration = std::format("narration {}", number);
    record.timings = {{"resolve_ms", 120}};
    return record;
}

nlohmann::json StateWithHealth(int health)
{
    return {{"player", {{"type", "character"}, {"fields", {{"health", health}}}}}};
}

} // namespace

TEST_CASE("RunStore commits turns with their resulting state", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turns"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));

    CHECK(store.LatestTurn() == 2);
    CHECK(store.StateAt(1) == StateWithHealth(9));
    CHECK(store.StateAt(2) == StateWithHealth(8));

    const std::optional<TurnRecord> first = store.Turn(1);
    REQUIRE(first.has_value());
    CHECK(first->input == "input 1");
    CHECK(first->resolve == nlohmann::json({{"intent", "climb"}}));
    CHECK(first->rolls.at(0).at("total") == 14);
    CHECK(first->applied.size() == 1);
    CHECK(first->rejected.empty());
    CHECK(first->narration == "narration 1");
    CHECK(first->timings.at("resolve_ms") == 120);

    const std::vector<TurnRecord> both = store.Turns(1, 2);
    REQUIRE(both.size() == 2);
    CHECK(both[0].number == 1);
    CHECK(both[1].number == 2);
    CHECK_FALSE(store.Turn(3).has_value());
}

TEST_CASE("RunStore only accepts the next turn number", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turn_order"), TestMeta(), TestInitialState());
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(2), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 0);

    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(1), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 1);
}

TEST_CASE("TruncateAfter undoes later turns and snapshots", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));
    store.CommitTurn(TestTurn(3), StateWithHealth(7));

    store.TruncateAfter(1);
    CHECK(store.LatestTurn() == 1);
    CHECK_FALSE(store.Turn(2).has_value());
    CHECK_THROWS_AS(store.StateAt(2), ContractError);
    CHECK(store.StateAt(1) == StateWithHealth(9));

    // The run continues from turn 1 as if turns 2 and 3 never happened.
    store.CommitTurn(TestTurn(2), StateWithHealth(5));
    CHECK(store.StateAt(2) == StateWithHealth(5));
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C2065: 'TurnRecord': undeclared identifier` (or a similar unknown-identifier error).

- [ ] **Step 3: Extend `src/store/RunStore.h`**

Add `#include <optional>` and `#include <vector>` to the includes. Add this struct directly after `struct RunMeta { ... };`:

```cpp
// Everything one committed turn produced, as stored in the turns table. JSON members hold the turn module's
// structured outputs verbatim.
struct TurnRecord {
    int number = 0;
    std::string input = {};
    nlohmann::json resolve = nlohmann::json::object(); // Resolve call output.
    nlohmann::json rolls = nlohmann::json::array();    // Check results.
    nlohmann::json applied = nlohmann::json::array();  // Applied changes.
    nlohmann::json rejected = nlohmann::json::array(); // Rejected changes with reasons.
    std::string narration = {};
    nlohmann::json timings = nlohmann::json::object();
};
```

Add these public methods directly after `nlohmann::json StateAt(int turn);`:

```cpp
    // Appends turn LatestTurn()+1 and the state it produced, in one transaction.
    // Throws ContractError if record.number is not LatestTurn()+1.
    void CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter);

    // nullopt when the turn does not exist.
    std::optional<TurnRecord> Turn(int number);

    // Turns in [fromTurn, toTurn], ascending.
    std::vector<TurnRecord> Turns(int fromTurn, int toTurn);

    // Undo: removes every turn, snapshot and note after `turn`, every turn embedding whose turn is after it,
    // and every summary that ends after it. Throws ContractError if turn < 0.
    void TruncateAfter(int turn);
```

- [ ] **Step 4: Implement the new methods in `src/store/RunStore.cpp`**

Insert these directly after the `RunStore::StateAt` definition:

```cpp
void RunStore::CommitTurn(const TurnRecord& record, const nlohmann::json& stateAfter)
{
    const int expected = LatestTurn() + 1;
    MORPHEUS_REQUIRE(record.number == expected,
                     std::format("CommitTurn: expected turn {}, got {}", expected, record.number));

    // The turn and its resulting state land together or not at all.
    _db.Transaction([&] {
        _db.Prepare("INSERT INTO turns (number, input, resolve, rolls, applied, rejected, narration, timings) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)")
            .Bind(1, record.number)
            .Bind(2, record.input)
            .Bind(3, record.resolve.dump())
            .Bind(4, record.rolls.dump())
            .Bind(5, record.applied.dump())
            .Bind(6, record.rejected.dump())
            .Bind(7, record.narration)
            .Bind(8, record.timings.dump())
            .Run();
        _db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (?1, ?2)")
            .Bind(1, record.number)
            .Bind(2, stateAfter.dump())
            .Run();
    });
}

std::optional<TurnRecord> RunStore::Turn(int number)
{
    std::vector<TurnRecord> found = Turns(number, number);
    if (found.empty()) {
        return std::nullopt;
    }
    return std::move(found.front());
}

std::vector<TurnRecord> RunStore::Turns(int fromTurn, int toTurn)
{
    Statement query = _db.Prepare("SELECT number, input, resolve, rolls, applied, rejected, narration, timings "
                                  "FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
    query.Bind(1, fromTurn).Bind(2, toTurn);

    std::vector<TurnRecord> records;
    while (query.Step()) {
        TurnRecord record;
        record.number = static_cast<int>(query.ColumnInt(0));
        record.input = query.ColumnText(1);
        record.resolve = nlohmann::json::parse(query.ColumnText(2));
        record.rolls = nlohmann::json::parse(query.ColumnText(3));
        record.applied = nlohmann::json::parse(query.ColumnText(4));
        record.rejected = nlohmann::json::parse(query.ColumnText(5));
        record.narration = query.ColumnText(6);
        record.timings = nlohmann::json::parse(query.ColumnText(7));
        records.push_back(std::move(record));
    }
    return records;
}

void RunStore::TruncateAfter(int turn)
{
    MORPHEUS_REQUIRE(turn >= 0, "TruncateAfter: turn must be >= 0");

    // Everything that happened after `turn` goes, in one transaction. Summaries that end at or before `turn`
    // only cover unchanged turns, so they stay.
    _db.Transaction([&] {
        _db.Prepare("DELETE FROM turns WHERE number > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM state_snapshots WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM summaries WHERE to_turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM notes WHERE turn > ?1").Bind(1, turn).Run();
        _db.Prepare("DELETE FROM embeddings WHERE kind = 'turn' AND CAST(ref_id AS INTEGER) > ?1").Bind(1, turn).Run();
    });
}
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/store tests/store
git commit -m "Add turn commits, snapshots and undo to RunStore" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

