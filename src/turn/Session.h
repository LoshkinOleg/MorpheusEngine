#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/MemoryUpkeep.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

constexpr std::size_t RETRIEVAL_MAX_HITS = 6; // Retrieval hits in the tail (T2), before retrieval_cap applies.
constexpr std::size_t LOOKUP_MAX_HITS = 2;    // Hits each assisted-mode lookup adds.
constexpr int LOOKUP_MAX_TOKENS = 192;        // Generation cap for the lookup call.
// Added to a turn's attempt for the seed of its second review, so that seed never equals a regenerated turn's first
// review seed (attempt + 1). PlayTurn requires attempt < REVIEW_RETRY_OFFSET, which keeps the sum within
// SamplingSeed's limit of 2^24.
constexpr int REVIEW_RETRY_OFFSET = 1 << 23;

// The sampling for one call from its settings.json values and the call's seed (core::SamplingSeed).
inference::Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed);

struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    // Every narration's pieces as they are written, the renarrate's included.
    inference::TokenCallback onNarrationToken = {};
    // The review found that the narration contradicts the outcome (`problems` are its reasons): the narration starts
    // over, and the new one streams through onNarrationToken. At most once per turn.
    std::function<void(const std::vector<std::string>& problems)> onNarrationRestarted = {};
};

// One open run: its world, settings, database and memory index, and the turn loop (architecture spec, "Turn flow").
// Every backend call happens inside these methods, on the caller's thread (the engine worker). Not thread-safe.
class Session {
private:
    core::World _world = {}; // Set by the constructor.
    core::Settings _settings = {};
    core::MemoryMode _mode = core::MemoryMode::Engine;
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index;     // Set by the constructor.
    context::Assembly _lastAssembly = {};
    bool _worldChanged = false;

public:
    // Starts a new run in `file` from the world's initial state, with `seed` for dice and sampling, and indexes the
    // lore. Throws StoreError if the file exists, and DataError if lore cannot be embedded (then no file is left).
    static Session Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                         std::uint64_t seed, inference::LlmBackend& backend);

    // Opens a run of `world`. Throws DataError if the run belongs to another world or its latest state no longer
    // validates against the world's schema. Edited world files set WorldChanged(); edited lore is re-embedded.
    static Session Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                        inference::LlmBackend& backend);

    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, (narration_check or
    // compact_transcript: review, and at most one renarrate with its own review), commit. Control characters other
    // than newline and tab are stripped from `input` and it is trimmed. Returns the committed record, or nullopt
    // when `stop` cancelled the turn. Throws TurnError for an empty input or one that does not fit, DataError when
    // summary_cap is too small for even a condensed summary, and InferenceError from the backend. A cancelled or
    // failed turn commits no turn, but memory upkeep its fit loop finished before the stop (summaries and their
    // history embeddings) is kept: it only covers committed turns.
    std::optional<core::TurnRecord> RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                            std::stop_token stop);

    // Removes the latest turn, with its notes and any summary covering it, from the run and from retrieval.
    // Throws TurnError when there is none.
    void Undo();

    // Plays the latest turn's input again as a new attempt: same dice, newly sampled text. If it is cancelled or
    // fails, the previous turn is put back with the summaries that covered it. Throws TurnError when there is no
    // turn.
    std::optional<core::TurnRecord> Regenerate(const TurnCallbacks& callbacks, std::stop_token stop);

    // Between-turn upkeep: folds or condenses while the transcript or the summary is over budget, then indexes
    // folded turns and notes. Returns false if `stop` cancelled it (it can simply run again later). Throws DataError
    // when summary_cap is too small for even a condensed summary.
    bool Maintain(std::stop_token stop);

    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    nlohmann::json State();                  // After the latest turn.
    std::vector<core::TurnRecord> History(); // Every committed turn, oldest first.
    std::vector<core::Summary> Summaries();
    std::vector<core::Note> Notes(); // Every committed note, by turn, then the order written.
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the context inspector.

private:
    Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
            inference::LlmBackend& backend);

    // RunTurn and Regenerate: plays turn LatestTurn()+1 for a cleaned input.
    std::optional<core::TurnRecord> PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                             std::stop_token stop);

    // One pass of the memory upkeep: the prompt for `hits` and the given tail parts, with the summaries and the
    // unfolded transcript read from the store as it is now, and the transcript entries it shows.
    FitPass Pass(const nlohmann::json& state, const std::vector<RetrievalHit>& hits, const std::string& input,
                 std::string_view instruction);

    // The shared memory upkeep for turn `turn`'s memory calls: a fold's summary joins the store, and the turns it
    // covers become searchable history.
    MemoryUpkeep Upkeep(int turn, int attempt);

    // The last turn covered by a summary, 0 when none.
    int FoldedThrough();
};

} // namespace morpheus::turn
