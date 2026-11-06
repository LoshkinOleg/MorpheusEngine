### Task 3: Store support for regenerate, notes and condensing

**Files:**
- Modify: `src/core/TurnRecord.h`, `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: the existing store.
- Produces:
  - `int core::TurnRecord::attempt`, stored in a new `turns.attempt` column. No runs exist, so `SCHEMA_VERSION` stays 1.
  - `store::EmbeddingKind::Note` (stored as `"note"`; `refId` is `"<turn>.<index>"`)
  - `Embeddings()` ordered numerically by `refId`
  - `TruncateAfter` also removes note embeddings after the turn
  - `void RunStore::ReplaceSummaries(const core::Summary& merged)`

- [ ] **Step 1: Write the failing tests**

Append to `tests/store/RunStoreTests.cpp`:

```cpp
TEST_CASE("A turn's attempt number round-trips", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_attempt"), TestMeta(), TestInitialState());
    morpheus::core::TurnRecord record;
    record.number = 1;
    record.attempt = 2;
    record.input = "I wait.";
    record.narration = "Time passes.";
    store.CommitTurn(record, TestInitialState());
    CHECK(store.Turn(1)->attempt == 2);
    CHECK(store.Turns(1, 1).at(0).attempt == 2);
}

TEST_CASE("ReplaceSummaries swaps every summary for one", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_replace_summaries"), TestMeta(), TestInitialState());
    store.AddSummary({1, 3, "first"});
    store.AddSummary({4, 6, "second"});
    store.ReplaceSummaries({1, 6, "both"});
    const std::vector<morpheus::core::Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 6);
    CHECK(summaries[0].text == "both");
    CHECK_THROWS_AS(store.ReplaceSummaries({3, 2, "backwards"}), morpheus::core::ContractError);
}

TEST_CASE("Turn and note embeddings come back in numeric order and go with their turns", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_note_embeddings"), TestMeta(), TestInitialState());
    for (const char* ref : {"10", "9", "2"}) {
        store.PutEmbedding({EmbeddingKind::Turn, ref, "embed-a", {1.0f}, "h"});
    }
    for (const char* ref : {"10.0", "2.1", "2.0"}) {
        store.PutEmbedding({EmbeddingKind::Note, ref, "embed-a", {1.0f}, "h"});
    }

    std::vector<std::string> turnRefs;
    for (const EmbeddingRow& row : store.Embeddings(EmbeddingKind::Turn, "embed-a")) {
        turnRefs.push_back(row.refId);
    }
    CHECK(turnRefs == std::vector<std::string>{"2", "9", "10"});

    store.TruncateAfter(5);
    std::vector<std::string> noteRefs;
    for (const EmbeddingRow& row : store.Embeddings(EmbeddingKind::Note, "embed-a")) {
        noteRefs.push_back(row.refId);
    }
    CHECK(noteRefs == std::vector<std::string>{"2.0", "2.1"});
    CHECK(store.Embeddings(EmbeddingKind::Turn, "embed-a").size() == 1);
}
```

Use the file's existing helpers `FreshDbPath`, `TestMeta` and `TestInitialState`. Add `<string>` and `<vector>` includes if missing.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `attempt`, `EmbeddingKind::Note` and `ReplaceSummaries` are undeclared.

- [ ] **Step 3: Implement**

`src/core/TurnRecord.h`, in `TurnRecord` after `number`:

```cpp
    int attempt = 0; // 0 for the first try; each regenerate adds 1 (new text sampling, same dice).
```

`src/store/RunStore.h`:
- The enum becomes `enum class EmbeddingKind { Lore, Turn, Note };`.
- The `EmbeddingRow` comment becomes: `// refId: lore subject for Lore, decimal turn number for Turn, "<turn>.<index>" for Note (index: the note's position among that turn's notes).`
- The `Embeddings` comment gains: "Ordered by the number refId starts with, then refId."
- The `TruncateAfter` comment: "...every turn and note embedding whose turn is after it...".
- Add after `AddSummary`:

```cpp
    // Condensing: replaces every summary with `merged` in one transaction. Throws ContractError unless
    // 1 <= merged.fromTurn <= merged.toTurn.
    void ReplaceSummaries(const core::Summary& merged);
```

`src/store/RunStore.cpp`:
- **Schema:** add `attempt INTEGER NOT NULL` to the `turns` table, after `number`.
- **Turn rows:** the `CommitTurn` INSERT writes `record.attempt`, and both turn SELECTs read it back into `attempt`. Bind and read it the same way as `number`.
- **`KindName`:** add `case EmbeddingKind::Note: return "note";`. The comment becomes "TruncateAfter's SQL relies on "turn" and "note"."
- **`TruncateAfter`:** the embeddings statement becomes:

```cpp
        _db.Prepare("DELETE FROM embeddings WHERE kind IN ('turn', 'note') AND CAST(ref_id AS INTEGER) > ?1")
            .Bind(1, turn)
            .Run();
```

  (`CAST('12.0' AS INTEGER)` is 12, so note refs work too.)
- **`Embeddings`:** the query ends with `ORDER BY CAST(ref_id AS INTEGER), ref_id`.
- **`ReplaceSummaries`:**

```cpp
void RunStore::ReplaceSummaries(const core::Summary& merged)
{
    MORPHEUS_REQUIRE(merged.fromTurn >= 1 && merged.fromTurn <= merged.toTurn,
                     "ReplaceSummaries: needs 1 <= fromTurn <= toTurn");
    Transaction transaction(_db);
    _db.Prepare("DELETE FROM summaries").Run();
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
        .Bind(1, merged.fromTurn)
        .Bind(2, merged.toTurn)
        .Bind(3, merged.text)
        .Run();
    transaction.Commit();
}
```

Match the file's existing `Transaction` and `Statement` usage, as in `AddSummary` and `CommitTurn`, if their spelling differs.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: PASS.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/core/TurnRecord.h src/store tests/store
git commit -m "Store turn attempts, note embeddings and condensed summaries" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---
