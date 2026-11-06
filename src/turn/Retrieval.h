#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"

namespace morpheus::turn {

constexpr float RETRIEVAL_MIN_SCORE = 0.25f;       // Cosine below this is noise (nomic-embed scale; tune in play).
constexpr float NAME_BOOST = 0.15f;                // Added once when a lore subject appears in the input or the scene.
constexpr std::size_t HISTORY_SNIPPET_CHARS = 600; // Input and narration shown per history hit, each.
constexpr std::size_t EMBED_TEXT_CHARS = 4000;     // Turn and note text embedded, so it fits the embedding context.

// One retrieved snippet for the tail (T2).
struct RetrievalHit {
    store::EmbeddingKind kind = store::EmbeddingKind::Lore;
    std::string refId = {};
    std::string text = {}; // As the prompt shows it.
    float score = 0.0f;
    int tokens = 0; // What its prompt line costs against the retrieval cap (set by Retrieve).
    // Why it matched beyond similarity: "name" (its lore subject is in the input), "scene" (in a scene value). Empty
    // for similarity alone.
    std::vector<std::string> reasons = {};
};

// What retrieval searches with, in parts, so Retrieve can tell why an entry matched.
struct RetrievalQuery {
    std::string input = {};              // The player's input, or an assisted-mode lookup's query.
    std::vector<std::string> scene = {}; // SceneOf's values.
    std::string digest = {};             // The previous turn's digest.

    // What is embedded: the input, the scene values joined by "; ", then the digest, one per line (empty parts left
    // out), cut to EMBED_TEXT_CHARS bytes. The input comes first, so it survives the cut.
    std::string Text() const;
};

// The scene text for `state` (architecture spec, "World data"), empty for a world without a scene block: the value
// at each declared path, then every entity other than present_at's whose field of present_at's field name is a
// string equal to present_at's value (trimmed, lower-cased), in id order: its id, then its other string fields' values
// (in field name order). Empty values are skipped; values are deduplicated case-insensitively (trimmed), keeping the
// first as written.
std::vector<std::string> SceneOf(const core::World& world, const nlohmann::json& state);

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
    // embedding where missing or stale. Throws ContractError if foldedThrough < 0, and InferenceError when the
    // backend cannot embed a text (the engine cuts them, so that is a runtime failure).
    void SyncHistory(store::RunStore& store, int foldedThrough, inference::LlmBackend& backend);

    // The best hits for `query` among `sources`. Each hit scores at least RETRIEVAL_MIN_SCORE: the cosine with the
    // embedding of query.Text(), plus NAME_BOOST, once, for a lore subject (lower-cased) contained in the input
    // (reason "name") or in a scene value (reason "scene"). The digest only enters the embedding. Hits come in score
    // order (ties by kind, then refId), at most maxHits of them, whose "- text\n" lines fit capTokens together. A hit
    // too big for the rest of the cap is skipped. No embedding call is made when nothing is searchable.
    std::vector<RetrievalHit> Retrieve(const RetrievalQuery& query, std::span<const store::EmbeddingKind> sources,
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
