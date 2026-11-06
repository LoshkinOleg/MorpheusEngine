### Task 8: ScriptedBackend and the memory index (retrieval)

**Files:**
- Create: `tests/fakes/ScriptedBackend.h`, `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`, `tests/turn/RetrievalTests.cpp`
- Modify: `tests/fakes/FakeBackend.h` (drop `final`), `src/turn/CMakeLists.txt` (add `Retrieval.cpp`)

**Interfaces:**
- Consumes:
  - `store::RunStore::{Embeddings, PutEmbedding, Turns, Notes}`
  - `core::ContentHash`, `core::CutUtf8`
  - `inference::LlmBackend::{Embed, EmbeddingModelId, CountTokens}`
- Produces:
  - `test::ScriptedBackend(int contextSize, Responder responder = {})`, with `Responder = std::function<std::string(const inference::Prompt&, const inference::GenerationSpec&)>`, plus `Calls()`, `EmbeddedTexts()`, `SetEmbedLimit(std::size_t)` and `SetPieceDelay(std::chrono::milliseconds)`
  - `struct turn::RetrievalHit { store::EmbeddingKind kind; std::string refId; std::string text; float score; }`
  - `turn::MemoryIndex(std::string documentPrefix)`, with `SyncLore(world, store, backend)`, `SyncHistory(store, foldedThrough, backend)`, and `Retrieve(query, sources, capTokens, maxHits, backend) const`
  - The constants `RETRIEVAL_MIN_SCORE`, `NAME_BOOST`, `HISTORY_SNIPPET_CHARS` and `EMBED_TEXT_CHARS`

- [ ] **Step 1: Write the test fake and the failing tests**

In `tests/fakes/FakeBackend.h`, change `class FakeBackend final` to `class FakeBackend`. Update its comment: "ScriptedBackend derives from it for turn-layer tests."

Create `tests/fakes/ScriptedBackend.h`:

```cpp
#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "fakes/FakeBackend.h"

namespace morpheus::test {

// A backend for turn-layer tests. Tokens are counted like FakeBackend. Complete asks the responder for the reply,
// streams it in pieces of about 4 bytes that never split a UTF-8 character (stopping early when `stop` is
// requested), and records the call. Embed maps a text to a bag of words (lower-cased [a-z0-9] runs hashed into
// EMBED_DIMS dimensions, L2-normalized), so texts sharing words score higher. Save/RestoreState keep nothing.
class ScriptedBackend final : public FakeBackend {
public:
    using Responder = std::function<std::string(const inference::Prompt&, const inference::GenerationSpec&)>;

    struct Call {
        inference::Prompt prompt = {};
        inference::GenerationSpec spec = {};
    };

    static constexpr std::size_t EMBED_DIMS = 64;

private:
    Responder _responder = {};
    std::vector<Call> _calls = {};
    int _embeddedTexts = 0;
    std::size_t _embedLimit = 0; // Longest text Embed accepts, in bytes; 0 means no limit.
    std::chrono::milliseconds _pieceDelay = std::chrono::milliseconds(0);

public:
    explicit ScriptedBackend(int contextSize, Responder responder = {})
        : FakeBackend(contextSize)
        , _responder(std::move(responder))
    {
    }

    const std::vector<Call>& Calls() const
    {
        return _calls;
    }

    // Texts embedded so far, across calls.
    int EmbeddedTexts() const
    {
        return _embeddedTexts;
    }

    // Embed throws InferenceError for a text longer than `bytes` (an embedding context that is too small).
    void SetEmbedLimit(std::size_t bytes)
    {
        _embedLimit = bytes;
    }

    // Sleeps before each streamed piece, so tests can cancel mid-reply.
    void SetPieceDelay(std::chrono::milliseconds delay)
    {
        _pieceDelay = delay;
    }

    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override
    {
        MORPHEUS_REQUIRE(static_cast<bool>(_responder), "ScriptedBackend: no responder for Complete");
        _calls.push_back({prompt, spec});
        const std::string reply = _responder(prompt, spec);

        inference::Completion completion;
        completion.promptTokens = CountPromptTokens(prompt);
        completion.tokensEvaluated = completion.promptTokens;
        std::size_t at = 0;
        while (at < reply.size()) {
            if (_pieceDelay.count() > 0) {
                std::this_thread::sleep_for(_pieceDelay);
            }
            if (stop.stop_requested()) {
                completion.cancelled = true;
                return completion;
            }
            std::size_t end = std::min(at + 4, reply.size());
            while (end < reply.size() && (static_cast<unsigned char>(reply[end]) & 0xC0) == 0x80) {
                ++end; // Never end a piece inside a UTF-8 character.
            }
            const std::string_view piece = std::string_view(reply).substr(at, end - at);
            completion.text += piece;
            ++completion.tokensGenerated;
            if (onToken) {
                onToken(piece);
            }
            at = end;
        }
        return completion;
    }

    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose) override
    {
        std::vector<std::vector<float>> vectors;
        for (const std::string& text : texts) {
            if (_embedLimit != 0 && text.size() > _embedLimit) {
                throw inference::InferenceError(std::format("text of {} bytes is over the embedding limit", text.size()));
            }
            ++_embeddedTexts;
            std::vector<float> vector(EMBED_DIMS, 0.0f);
            std::string word;
            auto flush = [&] {
                if (!word.empty()) {
                    core::ContentHash hash;
                    hash.Add(word);
                    vector[hash.Value() % EMBED_DIMS] += 1.0f;
                    word.clear();
                }
            };
            for (const char c : text) {
                const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
                    word += lower;
                } else {
                    flush();
                }
            }
            flush();
            float norm = 0.0f;
            for (const float value : vector) {
                norm += value * value;
            }
            if (norm > 0.0f) {
                for (float& value : vector) {
                    value /= std::sqrt(norm);
                }
            }
            vectors.push_back(std::move(vector));
        }
        return vectors;
    }

    std::string EmbeddingModelId() const override
    {
        return "scripted-embed";
    }

    std::vector<std::uint8_t> SaveState() override
    {
        return {};
    }

    void RestoreState(std::span<const std::uint8_t>) override
    {
    }
};

} // namespace morpheus::test
```

Add `#include <algorithm>`, `<span>`, `<stop_token>` and `<utility>`, and compute `std::sqrt(norm)` once before the loop if `/W4` objects.

Create `tests/turn/RetrievalTests.cpp`:

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <string>
#include <vector>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"
#include "fakes/ScriptedBackend.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr std::array<store::EmbeddingKind, 3> ALL_SOURCES = {store::EmbeddingKind::Lore, store::EmbeddingKind::Turn,
                                                             store::EmbeddingKind::Note};

core::World MiniWorld()
{
    return core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
}

store::RunStore NewRun(const std::string& name, const core::World& world)
{
    store::RunMeta meta;
    meta.worldId = world.id;
    meta.worldHash = world.contentHash;
    meta.modelId = "test-chat";
    meta.rngSeed = 1;
    return store::RunStore::Create(test::FreshTempDir(name) / "run.db", meta, world.initialState);
}

} // namespace

TEST_CASE("Lore retrieval ranks shared words first and skips always-on lore", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_lore", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("Do storms arrive at dusk here?", ALL_SOURCES, 1000, 6, backend);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].kind == store::EmbeddingKind::Lore);
    CHECK(hits[0].refId == "storm");
    CHECK(hits[0].text == "storm: Storms arrive at dusk.");
    for (const turn::RetrievalHit& hit : hits) {
        CHECK(hit.refId != "lighthouse"); // Always-on lore is in L1, never retrieved.
    }
}

TEST_CASE("Stored lore vectors are reused until the text or the prefix changes", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_reuse", world);
    test::ScriptedBackend backend(16384);

    turn::MemoryIndex(std::string("search_document: ")).SyncLore(world, store, backend);
    CHECK(backend.EmbeddedTexts() == 2); // keeper and storm.
    turn::MemoryIndex(std::string("search_document: ")).SyncLore(world, store, backend);
    CHECK(backend.EmbeddedTexts() == 2); // Reused from the store.

    core::World edited = world;
    edited.lore[2].data = "Storms arrive at dawn."; // lore.csv order: lighthouse, keeper, storm.
    turn::MemoryIndex(std::string("search_document: ")).SyncLore(edited, store, backend);
    CHECK(backend.EmbeddedTexts() == 3);
    turn::MemoryIndex(std::string("passage: ")).SyncLore(edited, store, backend);
    CHECK(backend.EmbeddedTexts() == 5);
}

TEST_CASE("A lore subject named in the input is boosted, and hits stay within the cap", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_boost", world);
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncLore(world, store, backend);

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("keeper", ALL_SOURCES, 1000, 6, backend);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].refId == "keeper");
    const std::vector<std::vector<float>> vectors = backend.Embed(
        {"keeper", "keeper: The keeper says \"hello\" to everyone."}, inference::EmbedPurpose::Document);
    float similarity = 0.0f;
    for (std::size_t i = 0; i < vectors[0].size(); ++i) {
        similarity += vectors[0][i] * vectors[1][i];
    }
    CHECK(hits[0].score == Catch::Approx(similarity + turn::NAME_BOOST));

    const int oneSnippet = backend.CountTokens("- storm: Storms arrive at dusk.\n");
    CHECK(index.Retrieve("storm keeper dusk everyone", ALL_SOURCES, oneSnippet, 6, backend).size() == 1);
    CHECK(index.Retrieve("storm keeper dusk everyone", ALL_SOURCES, 1000, 1, backend).size() == 1);
}

TEST_CASE("Only folded turns and notes are searchable history", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_history", world);
    core::TurnRecord first;
    first.number = 1;
    first.input = "I find a brass key.";
    first.narration = "The brass key is cold.";
    first.notes = {"The brass key opens the cellar."};
    store.CommitTurn(first, world.initialState);
    core::TurnRecord second;
    second.number = 2;
    second.input = "I find another brass key.";
    second.narration = "Two brass keys now.";
    store.CommitTurn(second, world.initialState);

    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncHistory(store, 1, backend); // Turn 2 is still in the transcript.

    const std::vector<turn::RetrievalHit> hits = index.Retrieve("brass key", ALL_SOURCES, 1000, 6, backend);
    bool foundTurn = false;
    bool foundNote = false;
    for (const turn::RetrievalHit& hit : hits) {
        CHECK(hit.refId != "2");
        if (hit.kind == store::EmbeddingKind::Turn && hit.refId == "1") {
            foundTurn = true;
            CHECK(hit.text == "Turn 1: I find a brass key. -> The brass key is cold.");
        }
        if (hit.kind == store::EmbeddingKind::Note && hit.refId == "1.0") {
            foundNote = true;
            CHECK(hit.text == "Note from turn 1: The brass key opens the cellar.");
        }
    }
    CHECK(foundTurn);
    CHECK(foundNote);
}

TEST_CASE("Lore that cannot be embedded or repeats a subject is a world error", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_errors", world);
    test::ScriptedBackend backend(16384);
    backend.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::MemoryIndex(std::string("p: ")).SyncLore(world, store, backend), core::DataError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("keeper")));

    test::ScriptedBackend roomy(16384);
    core::World doubled = world;
    doubled.lore.push_back({"storm", "Another storm.", false});
    CHECK_THROWS_MATCHES(turn::MemoryIndex(std::string("p: ")).SyncLore(doubled, store, roomy), core::DataError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("two entries")));
}
```

Add `#include <catch2/matchers/catch_matchers_exception.hpp>` if `MessageMatches` needs it; use the same includes as the existing `CHECK_THROWS_MATCHES` tests.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: the build fails. `turn/Retrieval.h` cannot be opened.

- [ ] **Step 3: Implement**

Create `src/turn/Retrieval.h`:

```cpp
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"

namespace morpheus::turn {

constexpr float RETRIEVAL_MIN_SCORE = 0.25f;       // Cosine below this is noise (nomic-embed scale; tune in play).
constexpr float NAME_BOOST = 0.15f;                // Added when a lore subject appears in the query.
constexpr std::size_t HISTORY_SNIPPET_CHARS = 600; // Input and narration shown per history hit, each.
constexpr std::size_t EMBED_TEXT_CHARS = 4000;     // Turn and note text embedded, so it fits the embedding context.

// One retrieved snippet for the tail (T2).
struct RetrievalHit {
    store::EmbeddingKind kind = store::EmbeddingKind::Lore;
    std::string refId = {};
    std::string text = {}; // As the prompt shows it.
    float score = 0.0f;
};

// Similarity search over a run's memory (architecture spec, turn flow step 1): lore that is not always on, folded
// turns, and notes. Turns still in the transcript are not searchable: they are already in the prompt. Vectors are
// stored in the RunStore with the content hash of the document prefix plus the embedded text, so edited lore, a
// changed prefix or another embedding model re-embeds instead of reusing a stale vector.
class MemoryIndex {
private:
    struct Entry {
        store::EmbeddingKind kind = store::EmbeddingKind::Lore;
        std::string refId = {};
        std::string name = {}; // Lore: the lower-cased subject, for the name boost. Empty otherwise.
        std::string text = {}; // The snippet the prompt shows.
        std::vector<float> vector = {};
    };
    struct Document {
        std::string refId = {};
        std::string text = {}; // Exactly what is embedded.
    };

    std::string _documentPrefix = {};
    std::vector<Entry> _entries = {};

public:
    explicit MemoryIndex(std::string documentPrefix);

    // Indexes every lore entry that is not always on as "subject: data", embedding entries one at a time where
    // the stored vector is missing or stale. Throws DataError naming the subject when an entry cannot be embedded
    // (too long for the embedding model) or a subject appears twice.
    void SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend);

    // Indexes turns 1..foldedThrough ("Turn N: input -> narration") and every note ("Note from turn N: text"),
    // embedding where missing or stale. Throws ContractError if foldedThrough < 0.
    void SyncHistory(store::RunStore& store, int foldedThrough, inference::LlmBackend& backend);

    // The best hits for `query` among `sources`. Each hit scores at least RETRIEVAL_MIN_SCORE (cosine, plus
    // NAME_BOOST for a lore subject the query names). Hits come in score order (ties by kind, then refId), at most
    // maxHits of them, whose "- text\n" lines fit capTokens together. A hit too big for the rest of the cap is
    // skipped. No embedding call is made when nothing is searchable.
    std::vector<RetrievalHit> Retrieve(std::string_view query, std::span<const store::EmbeddingKind> sources,
                                       int capTokens, std::size_t maxHits, inference::LlmBackend& backend) const;

private:
    // Vectors for `documents` of one kind, aligned with them: stored ones whose hash still matches, the rest
    // embedded one text per call and stored.
    std::vector<std::vector<float>> Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                            store::RunStore& store, inference::LlmBackend& backend) const;

    // Replaces every entry of `kind` with `entries`.
    void Replace(store::EmbeddingKind kind, std::vector<Entry> entries);
};

} // namespace morpheus::turn
```

Create `src/turn/Retrieval.cpp`:

```cpp
#include "turn/Retrieval.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "core/ContentHash.h"
#include "core/Errors.h"
#include "core/Text.h"

namespace morpheus::turn {

namespace {

// ASCII lower case, for matching lore subjects in a query.
std::string Lowered(std::string_view text)
{
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

} // namespace

MemoryIndex::MemoryIndex(std::string documentPrefix)
    : _documentPrefix(std::move(documentPrefix))
{
}

void MemoryIndex::SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend)
{
    std::vector<Document> documents;
    std::vector<Entry> entries;
    std::set<std::string> subjects;
    for (const core::LoreEntry& lore : world.lore) {
        if (lore.alwaysOn) {
            continue; // Always in L1.
        }
        if (!subjects.insert(lore.subject).second) {
            throw core::DataError(
                std::format("world '{}': lore.csv has two entries for subject '{}'", world.id, lore.subject));
        }
        const std::string text = lore.subject + ": " + lore.data;
        documents.push_back({lore.subject, text});
        entries.push_back({store::EmbeddingKind::Lore, lore.subject, Lowered(lore.subject), text, {}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Lore, documents, store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Lore, std::move(entries));
}

void MemoryIndex::SyncHistory(store::RunStore& store, int foldedThrough, inference::LlmBackend& backend)
{
    MORPHEUS_REQUIRE(foldedThrough >= 0, "SyncHistory: foldedThrough must be >= 0");

    std::vector<Document> turnDocuments;
    std::vector<Entry> turnEntries;
    if (foldedThrough > 0) {
        for (const core::TurnRecord& turn : store.Turns(1, foldedThrough)) {
            const std::string refId = std::to_string(turn.number);
            turnDocuments.push_back({refId, core::CutUtf8(turn.input + "\n" + turn.narration, EMBED_TEXT_CHARS)});
            turnEntries.push_back({store::EmbeddingKind::Turn, refId, {},
                                   std::format("Turn {}: {} -> {}", turn.number,
                                               core::CutUtf8(turn.input, HISTORY_SNIPPET_CHARS),
                                               core::CutUtf8(turn.narration, HISTORY_SNIPPET_CHARS)),
                                   {}});
        }
    }

    std::vector<Document> noteDocuments;
    std::vector<Entry> noteEntries;
    std::map<int, int> notesPerTurn;
    for (const core::Note& note : store.Notes()) {
        const std::string refId = std::format("{}.{}", note.turn, notesPerTurn[note.turn]++);
        noteDocuments.push_back({refId, core::CutUtf8(note.text, EMBED_TEXT_CHARS)});
        noteEntries.push_back({store::EmbeddingKind::Note, refId, {},
                               std::format("Note from turn {}: {}", note.turn, note.text), {}});
    }

    std::vector<std::vector<float>> turnVectors = Vectors(store::EmbeddingKind::Turn, turnDocuments, store, backend);
    for (std::size_t i = 0; i < turnEntries.size(); ++i) {
        turnEntries[i].vector = std::move(turnVectors[i]);
    }
    std::vector<std::vector<float>> noteVectors = Vectors(store::EmbeddingKind::Note, noteDocuments, store, backend);
    for (std::size_t i = 0; i < noteEntries.size(); ++i) {
        noteEntries[i].vector = std::move(noteVectors[i]);
    }
    Replace(store::EmbeddingKind::Turn, std::move(turnEntries));
    Replace(store::EmbeddingKind::Note, std::move(noteEntries));
}

std::vector<RetrievalHit> MemoryIndex::Retrieve(std::string_view query, std::span<const store::EmbeddingKind> sources,
                                                int capTokens, std::size_t maxHits,
                                                inference::LlmBackend& backend) const
{
    auto searched = [&sources](store::EmbeddingKind kind) {
        return std::find(sources.begin(), sources.end(), kind) != sources.end();
    };
    const bool anything = std::any_of(_entries.begin(), _entries.end(), [&](const Entry& e) { return searched(e.kind); });
    if (!anything || capTokens <= 0 || maxHits == 0 || query.empty()) {
        return {};
    }

    const std::vector<float> queryVector =
        backend.Embed({std::string(query)}, inference::EmbedPurpose::Query).at(0);
    const std::string loweredQuery = Lowered(query);
    std::vector<RetrievalHit> hits;
    for (const Entry& entry : _entries) {
        if (!searched(entry.kind)) {
            continue;
        }
        MORPHEUS_REQUIRE(entry.vector.size() == queryVector.size(), "Retrieve: vectors from different models");
        float score = 0.0f;
        for (std::size_t i = 0; i < queryVector.size(); ++i) {
            score += queryVector[i] * entry.vector[i]; // Both are L2-normalized: the dot product is the cosine.
        }
        if (!entry.name.empty() && loweredQuery.find(entry.name) != std::string::npos) {
            score += NAME_BOOST;
        }
        if (score >= RETRIEVAL_MIN_SCORE) {
            hits.push_back({entry.kind, entry.refId, entry.text, score});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RetrievalHit& a, const RetrievalHit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.refId < b.refId;
    });

    std::vector<RetrievalHit> kept;
    int used = 0;
    for (RetrievalHit& hit : hits) {
        if (kept.size() == maxHits) {
            break;
        }
        const int cost = backend.CountTokens("- " + hit.text + "\n");
        if (used + cost > capTokens) {
            continue;
        }
        used += cost;
        kept.push_back(std::move(hit));
    }
    return kept;
}

std::vector<std::vector<float>> MemoryIndex::Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                                     store::RunStore& store, inference::LlmBackend& backend) const
{
    const std::string model = backend.EmbeddingModelId();
    std::map<std::string, store::EmbeddingRow> stored;
    for (store::EmbeddingRow& row : store.Embeddings(kind, model)) {
        std::string refId = row.refId;
        stored.emplace(std::move(refId), std::move(row));
    }

    std::vector<std::vector<float>> vectors;
    vectors.reserve(documents.size());
    for (const Document& document : documents) {
        core::ContentHash hash;
        hash.Add(_documentPrefix);
        hash.Add(document.text);
        const auto it = stored.find(document.refId);
        if (it != stored.end() && it->second.contentHash == hash.Hex()) {
            vectors.push_back(it->second.vector);
            continue;
        }
        std::vector<float> vector;
        try {
            vector = backend.Embed({document.text}, inference::EmbedPurpose::Document).at(0);
        } catch (const inference::InferenceError& error) {
            throw core::DataError(std::format("{} '{}' cannot be embedded: {}",
                                              kind == store::EmbeddingKind::Lore ? "lore.csv entry" : "memory",
                                              document.refId, error.what()));
        }
        store.PutEmbedding({kind, document.refId, model, vector, hash.Hex()});
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

void MemoryIndex::Replace(store::EmbeddingKind kind, std::vector<Entry> entries)
{
    std::erase_if(_entries, [kind](const Entry& entry) { return entry.kind == kind; });
    _entries.insert(_entries.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
}

} // namespace morpheus::turn
```

Add `Retrieval.cpp` to `src/turn/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[retrieval]"`
Expected: PASS.

If a bag-of-words hash collision ever puts an unrelated lore entry above `RETRIEVAL_MIN_SCORE`, the "skips always-on lore" assertions still hold. Report a ranking flake instead of loosening a ranking assertion.

Then run `ctest --preset tests`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/turn tests/fakes tests/turn/RetrievalTests.cpp
git commit -m "Add the memory index for lore, history and note retrieval" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

