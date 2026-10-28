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
