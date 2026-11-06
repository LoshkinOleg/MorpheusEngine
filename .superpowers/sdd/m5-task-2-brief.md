### Task 2: The turn's digest in the record and the store

A turn keeps the last review's digest. The `turns` table gains its column, which bumps `SCHEMA_VERSION` to 2; that also covers the `turns.attempt` column M3 added without a bump. A version-1 run is refused with the existing error; there is no migration.

**Files:**
- Modify: `src/core/TurnRecord.h`, `src/store/RunStore.h`, `src/store/RunStore.cpp`
- Test: `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `std::string core::TurnRecord::digest` (default `{}`, after `narration`); `store::RunStore::SCHEMA_VERSION == 2`; `CommitTurn` stores and `Turn` / `Turns` read the digest.

- [ ] **Step 1: Write the failing tests**

Append to `tests/store/RunStoreTests.cpp`:

```cpp
TEST_CASE("A turn's digest round-trips", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_digest"), TestMeta(), TestInitialState());
    morpheus::core::TurnRecord record;
    record.number = 1;
    record.input = "I wait.";
    record.narration = "Time passes.";
    record.digest = "The player waited.";
    store.CommitTurn(record, TestInitialState());
    CHECK(store.Turn(1)->digest == "The player waited.");
    CHECK(store.Turns(1, 1).at(0).digest == "The player waited.");

    // A turn without a review stores an empty digest.
    morpheus::core::TurnRecord quiet;
    quiet.number = 2;
    quiet.input = "I wait again.";
    store.CommitTurn(quiet, TestInitialState());
    CHECK(store.Turn(2)->digest.empty());
}

TEST_CASE("RunStore refuses a version-1 run, naming the file and both versions", "[store]")
{
    CHECK(RunStore::SCHEMA_VERSION == 2);
    const std::filesystem::path file = FreshDbPath("store_version_1");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '1' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_AS(RunStore::Open(file), StoreError);
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version 1 is not the supported version 2"));
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: a compile error: `digest` is not a member of `morpheus::core::TurnRecord`.

- [ ] **Step 3: Add the digest to the record**

In `src/core/TurnRecord.h`, after `std::string narration = {};` add:

```cpp
    std::string digest = {}; // The last review's one or two sentence digest; empty when there was none.
```

- [ ] **Step 4: Store it**

In `src/store/RunStore.h`, replace `static constexpr int SCHEMA_VERSION = 1;` with:

```cpp
    // 2: turns.digest. It also covers turns.attempt, which M3 added without a bump. Older runs are refused.
    static constexpr int SCHEMA_VERSION = 2;
```

and replace the comment above `static RunStore Open(...)` with:

```cpp
    // Opens an existing run. Throws StoreError if it is missing or was written with another schema version (there
    // is no migration), or its meta (rng_seed, settings) is damaged.
```

In `src/store/RunStore.cpp`, replace the `turns` table in `SCHEMA_SQL` with:

```sql
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    attempt   INTEGER NOT NULL,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    checks    TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    digest    TEXT NOT NULL,
    timings   TEXT NOT NULL
);
```

In `CommitTurn`, replace the turn insert with:

```cpp
        _db.Prepare("INSERT INTO turns (number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                    "timings) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)")
            .Bind(1, record.number)
            .Bind(2, record.attempt)
            .Bind(3, record.input)
            .Bind(4, record.resolve.dump())
            .Bind(5, checks.dump())
            .Bind(6, applied.dump())
            .Bind(7, rejected.dump())
            .Bind(8, record.narration)
            .Bind(9, record.digest)
            .Bind(10, record.timings.dump())
            .Run();
```

In `Turns`, replace the query with:

```cpp
    Statement query = _db.Prepare("SELECT number, attempt, input, resolve, checks, applied, rejected, narration, digest, "
                                  "timings FROM turns WHERE number BETWEEN ?1 AND ?2 ORDER BY number");
```

and replace

```cpp
        record.narration = query.ColumnText(7);
        record.timings = parseColumn(8, "timings");
```

with

```cpp
        record.narration = query.ColumnText(7);
        record.digest = query.ColumnText(8);
        record.timings = parseColumn(9, "timings");
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/core/TurnRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/RunStoreTests.cpp
git commit -m "Store each turn's digest and move the run schema to version 2" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
