### Task 3: Retrieval by witness, and summaries as a searchable source

**Files:**
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`
- Modify: `src/turn/HostedSession.cpp` (`SyncHistory` passes witnesses and indexes summaries; `RunTurn` retrieves for the speaker)
- Test: `tests/turn/RetrievalTests.cpp`

**Interfaces:**
- Consumes: `store::EmbeddingKind::Summary`, `RunStore::HostedSummaries()` (Task 1).
- Produces:
  - `struct turn::WitnessedEntry { context::TranscriptEntry entry = {}; std::vector<std::string> witnesses = {}; };`
  - `void MemoryIndex::SyncEntries(const std::vector<WitnessedEntry>& folded, store::RunStore& store, inference::LlmBackend& backend)` (replaces the `TranscriptEntry` overload).
  - `void MemoryIndex::SyncSummaries(const std::vector<core::Summary>& summaries, store::RunStore& store, inference::LlmBackend& backend)`: replaces every summary entry; each embedded as its text cut to `EMBED_TEXT_CHARS`, shown as `Memory of turns A-B: <text>` (text cut to `2 * HISTORY_SNIPPET_CHARS`), refId the summary's id in decimal, owned by `summary.actorId`.
  - `MemoryIndex::Retrieve(query, sources, capTokens, maxHits, backend, std::optional<std::string_view> witness = std::nullopt)`: with a witness, a turn entry is searched only when the witness is among its witnesses, and a summary entry only when the witness owns it. Lore and notes are unaffected. Without one, behaviour is unchanged.

- [ ] **Step 1: Write the failing test**

Add to `tests/turn/RetrievalTests.cpp` (add the includes it needs: `<array>`, `<set>`, `<utility>`, `"context/Transcript.h"`, `"core/TurnRecord.h"`):

```cpp
TEST_CASE("Retrieve for a witness returns only the turns it witnessed and the summaries it owns", "[retrieval][memory]")
{
    test::ScriptedBackend backend(16384);
    store::RunStore store = store::RunStore::Create(test::FreshTempDir("retrieval_witness") / "run.db",
                                                    store::RunMeta{}, nlohmann::json::object());
    turn::MemoryIndex index("search_document: ");
    auto turnEntry = [](int number, const std::string& fact) {
        context::TranscriptEntry entry;
        entry.turn = number;
        entry.fact = fact;
        return entry;
    };
    index.SyncEntries({{turnEntry(1, "bridge one"), {"hulda", "saadia"}}, {turnEntry(2, "bridge two"), {"hulda"}}},
                      store, backend);
    core::Summary mine{1, 1, "bridge three"};
    mine.actorId = "saadia";
    mine.id = 7;
    core::Summary theirs{2, 2, "bridge four"};
    theirs.actorId = "hulda";
    theirs.id = 8;
    index.SyncSummaries({mine, theirs}, store, backend);

    const std::array<store::EmbeddingKind, 2> sources = {store::EmbeddingKind::Turn, store::EmbeddingKind::Summary};
    turn::RetrievalQuery query;
    query.input = "bridge";
    // The hits as (kind, refId) pairs.
    auto found = [](const std::vector<turn::RetrievalHit>& hits) {
        std::set<std::pair<store::EmbeddingKind, std::string>> pairs;
        for (const turn::RetrievalHit& hit : hits) {
            pairs.insert({hit.kind, hit.refId});
        }
        return pairs;
    };
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend)).size() == 4);
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend, "saadia")) ==
          std::set<std::pair<store::EmbeddingKind, std::string>>{{store::EmbeddingKind::Turn, "1"},
                                                                 {store::EmbeddingKind::Summary, "7"}});
    CHECK(found(index.Retrieve(query, sources, 1000, 10, backend, "belethor")).empty());

    // A summary is shown under its range.
    const std::vector<turn::RetrievalHit> hulda = index.Retrieve(query, sources, 1000, 10, backend, "hulda");
    const bool shown = std::any_of(hulda.begin(), hulda.end(), [](const turn::RetrievalHit& hit) {
        return hit.kind == store::EmbeddingKind::Summary && hit.text == "Memory of turns 2-2: bridge four";
    });
    CHECK(shown);
}
```

Update the file's existing calls of `SyncEntries` to the `WitnessedEntry` form (witnesses empty: they are searched only without a witness filter).

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset tests`
Expected: a compile error (`SyncSummaries` is not a member of `MemoryIndex`).

- [ ] **Step 3: The index**

In `src/turn/Retrieval.h`, before `class MemoryIndex`:

```cpp
// A hosted run's folded turn as searchable history, with who witnessed it (its speaker and listeners).
struct WitnessedEntry {
    context::TranscriptEntry entry = {};
    std::vector<std::string> witnesses = {};
};
```

`Entry` gains, last: `std::vector<std::string> witnesses = {}; // Turn: its speaker and listeners. Summary: its owner. Empty otherwise.` Declare `SyncEntries(const std::vector<WitnessedEntry>&, ...)`, `SyncSummaries(...)` and the `witness` parameter of `Retrieve`, with comments as in **Produces**.

In `src/turn/Retrieval.cpp`:

```cpp
void MemoryIndex::SyncEntries(const std::vector<WitnessedEntry>& folded, store::RunStore& store,
                              inference::LlmBackend& backend)
{
    // Each folded turn under its run-wide number, with its witnesses: its fact is what is embedded and, cut shorter,
    // what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const WitnessedEntry& witnessed : folded) {
        const context::TranscriptEntry& turn = witnessed.entry;
        const std::string refId = std::to_string(turn.turn);
        documents.push_back({refId, core::CutUtf8(turn.fact, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Turn, refId, {},
                           std::format("Turn {}: {}", turn.turn, core::CutUtf8(turn.fact, 2 * HISTORY_SNIPPET_CHARS)),
                           {}, witnessed.witnesses});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Turn, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Turn, std::move(entries));
}

void MemoryIndex::SyncSummaries(const std::vector<core::Summary>& summaries, store::RunStore& store,
                                inference::LlmBackend& backend)
{
    // Each NPC summary under its id, owned by its NPC: its text is what is embedded and, cut shorter, what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const core::Summary& summary : summaries) {
        MORPHEUS_REQUIRE(!summary.actorId.empty() && summary.id > 0,
                         "MemoryIndex::SyncSummaries: a stored NPC summary is needed");
        const std::string refId = std::to_string(summary.id);
        documents.push_back({refId, core::CutUtf8(summary.text, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Summary, refId, {},
                           std::format("Memory of turns {}-{}: {}", summary.fromTurn, summary.toTurn,
                                       core::CutUtf8(summary.text, 2 * HISTORY_SNIPPET_CHARS)),
                           {}, {summary.actorId}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Summary, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Summary, std::move(entries));
}
```

In `Retrieve`, the `searched` lambda also applies the witness, and the `anything` check uses it, so a witness with nothing it may see makes no embedding call:

```cpp
    // An entry is searched when its kind is one of the sources and, for a witness, when it may see it: a turn it
    // witnessed, or a summary it owns. Lore and notes are everyone's.
    auto searched = [&sources, &witness](const Entry& entry) {
        if (std::find(sources.begin(), sources.end(), entry.kind) == sources.end()) {
            return false;
        }
        if (!witness || (entry.kind != store::EmbeddingKind::Turn && entry.kind != store::EmbeddingKind::Summary)) {
            return true;
        }
        return std::find(entry.witnesses.begin(), entry.witnesses.end(), *witness) != entry.witnesses.end();
    };
```

(`entry.witnesses` holds `std::string`; compare with `std::string_view` by `std::find_if(... [&](const std::string& id) { return id == *witness; })` if `std::find` does not compile.) Update the two uses of `searched` to pass the entry.

- [ ] **Step 4: The hosted session indexes witnesses and summaries, and retrieves for the speaker**

In `src/turn/HostedSession.cpp`:

- `HOSTED_SOURCES` becomes `{Lore, Turn, Summary}` (size 3).
- `SyncHistory` builds `WitnessedEntry`s: for each conversation, its folded entries (`Entries(conversation, true)`) with each turn's witnesses (`turn.speakerId` plus `turn.listenerIds`, from `_store.HostedTurns(conversation.id)` by turn number), then calls `_index.SyncEntries(folded, ...)` and `_index.SyncSummaries(_store.HostedSummaries(), _store, *_pBackend)`.
- `RunTurn`'s `_index.Retrieve(...)` passes `request.speakerId` as the witness.
- The `storeCondensed` of the scope (Task 2) calls `SyncHistory()` after `CondenseActorSummaries`, so the condensed summary is searchable.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[retrieval]"`, `"[hosted]"`, `"[turn]"`, `"[replay]"`.
Expected: `All tests passed` for each.

- [ ] **Step 6: Commit**

```bash
git add src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/HostedSession.cpp tests/turn/RetrievalTests.cpp
git commit -m "Retrieval by witness: folded turns carry their witnesses, NPC summaries are searchable by their owner"
```

---

