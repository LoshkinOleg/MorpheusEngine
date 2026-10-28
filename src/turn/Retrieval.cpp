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
