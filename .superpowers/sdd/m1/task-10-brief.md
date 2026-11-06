### Task 10: Summaries, notes and embeddings

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `RunStore`, `TurnRecord`, `TruncateAfter` (Task 9).
- Produces (namespace `morpheus::store`):
  - `struct Summary { int fromTurn; int toTurn; std::string text; }`
  - `struct Note { int turn; std::string text; }`
  - `enum class EmbeddingKind { Lore, Turn }`
  - `struct EmbeddingRow { EmbeddingKind kind; std::string refId; std::string model; std::vector<float> vector; }`
  - `void RunStore::AddSummary(const Summary&)`, `std::vector<Summary> RunStore::Summaries()`
  - `void RunStore::AddNote(const Note&)`, `std::vector<Note> RunStore::Notes()`
  - `void RunStore::PutEmbedding(const EmbeddingRow&)`, `std::vector<EmbeddingRow> RunStore::Embeddings(EmbeddingKind, const std::string& model)`
- Convention: a turn embedding's `refId` is the turn number in decimal ("12"). A lore embedding's `refId` is the lore subject.

- [ ] **Step 1: Append the failing tests to `tests/store/RunStoreTests.cpp`**

Add `#include <set>` to the includes at the top of the file. Then append:

```cpp
TEST_CASE("RunStore keeps summaries and notes in order", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_summaries"), TestMeta(), TestInitialState());
    store.AddSummary({5, 8, "second fold"});
    store.AddSummary({1, 4, "first fold"});
    store.AddNote({3, "the captain distrusts Marta"});
    store.AddNote({1, "storm season"});

    const std::vector<Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].text == "first fold");
    CHECK(summaries[1].fromTurn == 5);
    CHECK(summaries[1].toTurn == 8);

    const std::vector<Note> notes = store.Notes();
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].turn == 1);
    CHECK(notes[1].text == "the captain distrusts Marta");

    CHECK_THROWS_AS(store.AddSummary({4, 2, "backwards"}), ContractError);
}

TEST_CASE("RunStore stores embeddings per kind and model", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_embeddings"), TestMeta(), TestInitialState());
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {0.25f, -1.5f, 3.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}}); // Replaces the previous vector.
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-b", {2.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "1", "embed-a", {0.5f, -0.125f}});

    const std::vector<EmbeddingRow> lore = store.Embeddings(EmbeddingKind::Lore, "embed-a");
    REQUIRE(lore.size() == 1);
    CHECK(lore[0].refId == "boarding");
    CHECK(lore[0].vector == std::vector<float>{1.0f});

    const std::vector<EmbeddingRow> turns = store.Embeddings(EmbeddingKind::Turn, "embed-a");
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].kind == EmbeddingKind::Turn);
    CHECK(turns[0].vector == std::vector<float>{0.5f, -0.125f});

    CHECK_THROWS_AS(store.PutEmbedding({EmbeddingKind::Turn, "2", "embed-a", {}}), ContractError);
}

TEST_CASE("TruncateAfter also undoes later summaries, notes and turn embeddings", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate_memory"), TestMeta(), TestInitialState());
    for (int turn = 1; turn <= 5; ++turn) {
        store.CommitTurn(TestTurn(turn), StateWithHealth(10 - turn));
    }
    store.AddSummary({1, 3, "kept"});
    store.AddSummary({4, 5, "dropped"});
    store.AddNote({3, "kept"});
    store.AddNote({4, "dropped"});
    store.PutEmbedding({EmbeddingKind::Turn, "3", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "4", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}});

    store.TruncateAfter(3);

    REQUIRE(store.Summaries().size() == 1);
    CHECK(store.Summaries()[0].text == "kept");
    REQUIRE(store.Notes().size() == 1);
    CHECK(store.Notes()[0].text == "kept");

    std::set<std::string> turnRefs;
    for (const EmbeddingRow& row : store.Embeddings(EmbeddingKind::Turn, "embed-a")) {
        turnRefs.insert(row.refId);
    }
    CHECK(turnRefs == std::set<std::string>{"3"});
    CHECK(store.Embeddings(EmbeddingKind::Lore, "embed-a").size() == 1);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `'Summary': undeclared identifier` (or a similar unknown-identifier error).

- [ ] **Step 3: Extend `src/store/RunStore.h`**

Add these types directly after `struct TurnRecord { ... };`:

```cpp
// A folded range of turns (story summary, layer L2).
struct Summary {
    int fromTurn = 0;
    int toTurn = 0;
    std::string text = {};
};

// An append-only memory written by the model in assisted memory mode.
struct Note {
    int turn = 0;
    std::string text = {};
};

enum class EmbeddingKind { Lore, Turn };

// refId: lore subject for Lore, decimal turn number for Turn.
struct EmbeddingRow {
    EmbeddingKind kind = EmbeddingKind::Lore;
    std::string refId = {};
    std::string model = {};
    std::vector<float> vector = {};
};
```

Add these public methods directly after `void TruncateAfter(int turn);`:

```cpp
    // Throws ContractError unless 1 <= fromTurn <= toTurn.
    void AddSummary(const Summary& summary);

    // Ordered by fromTurn.
    std::vector<Summary> Summaries();

    void AddNote(const Note& note);

    // Ordered by turn, then insertion.
    std::vector<Note> Notes();

    // Inserts or replaces the vector for (kind, refId, model). Throws ContractError on an empty vector.
    void PutEmbedding(const EmbeddingRow& row);

    // Every embedding of one kind produced by one embedding model.
    std::vector<EmbeddingRow> Embeddings(EmbeddingKind kind, const std::string& model);
```

- [ ] **Step 4: Implement the new methods in `src/store/RunStore.cpp`**

Inside the existing anonymous namespace, after `SCHEMA_SQL`, add:

```cpp
// Stored spelling of an embedding kind. TruncateAfter's SQL relies on "turn".
const char* KindName(EmbeddingKind kind)
{
    switch (kind) {
    case EmbeddingKind::Lore:
        return "lore";
    case EmbeddingKind::Turn:
        return "turn";
    }
    throw morpheus::core::ContractError("KindName: unhandled EmbeddingKind");
}
```

Insert these directly after the `RunStore::TruncateAfter` definition:

```cpp
void RunStore::AddSummary(const Summary& summary)
{
    MORPHEUS_REQUIRE(summary.fromTurn >= 1 && summary.fromTurn <= summary.toTurn,
                     std::format("AddSummary: bad turn range {}..{}", summary.fromTurn, summary.toTurn));
    _db.Prepare("INSERT INTO summaries (from_turn, to_turn, text) VALUES (?1, ?2, ?3)")
        .Bind(1, summary.fromTurn)
        .Bind(2, summary.toTurn)
        .Bind(3, summary.text)
        .Run();
}

std::vector<Summary> RunStore::Summaries()
{
    Statement query = _db.Prepare("SELECT from_turn, to_turn, text FROM summaries ORDER BY from_turn, id");
    std::vector<Summary> summaries;
    while (query.Step()) {
        summaries.push_back({static_cast<int>(query.ColumnInt(0)), static_cast<int>(query.ColumnInt(1)),
                             query.ColumnText(2)});
    }
    return summaries;
}

void RunStore::AddNote(const Note& note)
{
    _db.Prepare("INSERT INTO notes (turn, text) VALUES (?1, ?2)").Bind(1, note.turn).Bind(2, note.text).Run();
}

std::vector<Note> RunStore::Notes()
{
    Statement query = _db.Prepare("SELECT turn, text FROM notes ORDER BY turn, id");
    std::vector<Note> notes;
    while (query.Step()) {
        notes.push_back({static_cast<int>(query.ColumnInt(0)), query.ColumnText(1)});
    }
    return notes;
}

void RunStore::PutEmbedding(const EmbeddingRow& row)
{
    MORPHEUS_REQUIRE(!row.vector.empty(), "PutEmbedding: vector must not be empty");
    _db.Prepare("INSERT OR REPLACE INTO embeddings (kind, ref_id, model, vector) VALUES (?1, ?2, ?3, ?4)")
        .Bind(1, KindName(row.kind))
        .Bind(2, row.refId)
        .Bind(3, row.model)
        .Bind(4, row.vector)
        .Run();
}

std::vector<EmbeddingRow> RunStore::Embeddings(EmbeddingKind kind, const std::string& model)
{
    Statement query = _db.Prepare("SELECT ref_id, vector FROM embeddings WHERE kind = ?1 AND model = ?2 ORDER BY ref_id");
    query.Bind(1, KindName(kind)).Bind(2, model);

    std::vector<EmbeddingRow> rows;
    while (query.Step()) {
        rows.push_back({kind, query.ColumnText(0), model, query.ColumnFloats(1)});
    }
    return rows;
}
```

- [ ] **Step 5: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/store tests/store
git commit -m "Add summaries, notes and embeddings to RunStore" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

